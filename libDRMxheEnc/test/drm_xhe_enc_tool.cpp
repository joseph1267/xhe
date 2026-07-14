/* -----------------------------------------------------------------------------
   drm-xhe-enc — DRM xHE-AAC encoder CLI.

   Usage: drm-xhe-enc <input.wav> <output_prefix> [options]
     -b <kbps>    bit rate in kbit/s, 12..128 (default 64)
     -sfms <ms>   DRM super-frame duration: 400 (DRM30) or 100 (DRM+).
                  Default: 400 if the frame size fits the 12-bit frame-border
                  directory, otherwise automatic fallback to 100.
     -sbr <r>     eSBR ratio: 0 (off), 83 (8:3), 21 (2:1, default), 41 (4:1)

   Outputs:
     <prefix>.drm  broadcast super-frame stream (fixed frame size =
                   bitRate * sfms / 8000 bytes)
     <prefix>.m4a  ISOBMFF container, esds ASC declares AOT 42 (USAC)
     <prefix>.cfg  raw AudioSpecificConfig (feed to drm-xhe-dec -c)

   Frame-size safety: the DRM directory carries 12-bit frame-border indices
   (two values reserved), so a super frame may be at most
   TPENC_DRM_MAX_SF_BYTES (4093) bytes. 400 ms frames therefore top out
   near 81 kbit/s; higher rates use 100 ms frames (as DRM+ does). Requesting
   a combination that cannot be represented is a hard error, and
   drmSfWriter_Init() re-validates the bound independently.
   -------------------------------------------------------------------------- */

#include "drm_xhe_enc.h"
#include "tpenc_drm.h"
#include "FDK_bitstream.h"
#include "drm_xhe_tool_common.h"

extern "C" {
#include "wavreader.h"
}

#define AU_BUF_BYTES (32768)  /* power of 2; > DRM_XHE_ENC_INFO::maxAuBytes */
#define SF_BUF_BYTES (32768)  /* drained after every AddAu/Flush; must hold
                                 the frames one max-size AU can emit */

static UCHAR auBuf[AU_BUF_BYTES];
static UCHAR sfBuf[SF_BUF_BYTES];
static INT_PCM pcmBuf[8192];

/* ---- streaming M4A writer: ftyp+mdat during encode, moov at finish ---- */

typedef struct {
  FILE *f;
  long mdatPos;
  long firstSample;
  UINT count;
  UINT *sizes;
  UINT cap;
} M4A_WRITER;

static long m4aBoxBegin(FILE *f, const char *type) {
  long pos = ftell(f);
  beW32(f, 0);
  fwrite(type, 1, 4, f);
  return pos;
}
static void m4aBoxEnd(FILE *f, long pos) {
  long end = ftell(f);
  fseek(f, pos, SEEK_SET);
  beW32(f, (unsigned int)(end - pos));
  fseek(f, end, SEEK_SET);
}

static int m4aBegin(M4A_WRITER *m, const char *path) {
  memset(m, 0, sizeof(*m));
  m->f = fopen(path, "wb");
  if (!m->f) return -1;
  long b = m4aBoxBegin(m->f, "ftyp");
  fwrite("M4A ", 1, 4, m->f);
  beW32(m->f, 0);
  fwrite("M4A isommp42", 1, 12, m->f);
  m4aBoxEnd(m->f, b);
  m->mdatPos = m4aBoxBegin(m->f, "mdat");
  m->firstSample = ftell(m->f);
  return 0;
}

static int m4aAddSample(M4A_WRITER *m, const UCHAR *au, UINT len) {
  if (m->count >= m->cap) {
    UINT newCap = (m->cap == 0) ? 1024 : m->cap * 2;
    UINT *p = (UINT *)realloc(m->sizes, newCap * sizeof(UINT));
    if (!p) return -1;
    m->sizes = p;
    m->cap = newCap;
  }
  fwrite(au, 1, len, m->f);
  m->sizes[m->count++] = len;
  return 0;
}

static void m4aFinish(M4A_WRITER *m, const UCHAR *asc, UINT ascLen, UINT sampleRate,
                      UINT channels, UINT frameDur, UINT avgBitrate) {
  FILE *f = m->f;
  unsigned int duration = m->count * frameDur;
  long b, b2, b3, b4, b5, b6;

  m4aBoxEnd(f, m->mdatPos);

  b = m4aBoxBegin(f, "moov");
  {
    long mvhd = m4aBoxBegin(f, "mvhd");
    beW32(f, 0);
    beW32(f, 0); beW32(f, 0);
    beW32(f, sampleRate);
    beW32(f, duration);
    beW32(f, 0x00010000); beW16(f, 0x0100); beW16(f, 0);
    beW32(f, 0); beW32(f, 0);
    beW32(f, 0x00010000); beW32(f, 0); beW32(f, 0);
    beW32(f, 0); beW32(f, 0x00010000); beW32(f, 0);
    beW32(f, 0); beW32(f, 0); beW32(f, 0x40000000);
    for (int i = 0; i < 6; i++) beW32(f, 0);
    beW32(f, 2);
    m4aBoxEnd(f, mvhd);

    b2 = m4aBoxBegin(f, "trak");
    {
      long tkhd = m4aBoxBegin(f, "tkhd");
      beW32(f, 7);
      beW32(f, 0); beW32(f, 0);
      beW32(f, 1);
      beW32(f, 0);
      beW32(f, duration);
      beW32(f, 0); beW32(f, 0);
      beW16(f, 0); beW16(f, 0); beW16(f, 0x0100); beW16(f, 0);
      beW32(f, 0x00010000); beW32(f, 0); beW32(f, 0);
      beW32(f, 0); beW32(f, 0x00010000); beW32(f, 0);
      beW32(f, 0); beW32(f, 0); beW32(f, 0x40000000);
      beW32(f, 0); beW32(f, 0);
      m4aBoxEnd(f, tkhd);

      b3 = m4aBoxBegin(f, "mdia");
      {
        long mdhd = m4aBoxBegin(f, "mdhd");
        beW32(f, 0);
        beW32(f, 0); beW32(f, 0);
        beW32(f, sampleRate);
        beW32(f, duration);
        beW16(f, 0x55C4); beW16(f, 0);
        m4aBoxEnd(f, mdhd);

        long hdlr = m4aBoxBegin(f, "hdlr");
        beW32(f, 0);
        beW32(f, 0);
        fwrite("soun", 1, 4, f);
        beW32(f, 0); beW32(f, 0); beW32(f, 0);
        fwrite("SoundHandler", 1, 13, f);
        m4aBoxEnd(f, hdlr);

        b4 = m4aBoxBegin(f, "minf");
        {
          long smhd = m4aBoxBegin(f, "smhd");
          beW32(f, 0);
          beW32(f, 0);
          m4aBoxEnd(f, smhd);

          long dinf = m4aBoxBegin(f, "dinf");
          long dref = m4aBoxBegin(f, "dref");
          beW32(f, 0);
          beW32(f, 1);
          long url = m4aBoxBegin(f, "url ");
          beW32(f, 1);
          m4aBoxEnd(f, url);
          m4aBoxEnd(f, dref);
          m4aBoxEnd(f, dinf);

          b5 = m4aBoxBegin(f, "stbl");
          {
            long stsd = m4aBoxBegin(f, "stsd");
            beW32(f, 0);
            beW32(f, 1);
            b6 = m4aBoxBegin(f, "mp4a");
            for (int i = 0; i < 6; i++) fputc(0, f);
            beW16(f, 1);
            beW32(f, 0); beW32(f, 0);
            beW16(f, channels);
            beW16(f, 16);
            beW16(f, 0); beW16(f, 0);
            beW32(f, sampleRate << 16);
            {
              long esds = m4aBoxBegin(f, "esds");
              beW32(f, 0);
              UINT dsiLen = ascLen;
              UINT dcdLen = 13 + 2 + dsiLen;
              UINT esLen = 3 + 2 + dcdLen + 3;
              fputc(0x03, f); fputc((int)esLen, f);
              beW16(f, 0); fputc(0, f);
              fputc(0x04, f); fputc((int)dcdLen, f);
              fputc(0x40, f); /* OTI: MPEG-4 Audio */
              fputc(0x15, f); /* audio stream */
              fputc(0, f); beW16(f, 0);
              beW32(f, avgBitrate);
              beW32(f, avgBitrate);
              fputc(0x05, f); fputc((int)dsiLen, f);
              fwrite(asc, 1, ascLen, f);
              fputc(0x06, f); fputc(1, f); fputc(0x02, f);
              m4aBoxEnd(f, esds);
            }
            m4aBoxEnd(f, b6);
            m4aBoxEnd(f, stsd);

            long stts = m4aBoxBegin(f, "stts");
            beW32(f, 0);
            beW32(f, 1);
            beW32(f, m->count);
            beW32(f, frameDur);
            m4aBoxEnd(f, stts);

            long stsc = m4aBoxBegin(f, "stsc");
            beW32(f, 0);
            beW32(f, 1);
            beW32(f, 1); beW32(f, m->count); beW32(f, 1);
            m4aBoxEnd(f, stsc);

            long stsz = m4aBoxBegin(f, "stsz");
            beW32(f, 0);
            beW32(f, 0);
            beW32(f, m->count);
            for (UINT i = 0; i < m->count; i++) beW32(f, m->sizes[i]);
            m4aBoxEnd(f, stsz);

            long stco = m4aBoxBegin(f, "stco");
            beW32(f, 0);
            beW32(f, 1);
            beW32(f, (unsigned int)m->firstSample);
            m4aBoxEnd(f, stco);
          }
          m4aBoxEnd(f, b5);
        }
        m4aBoxEnd(f, b4);
      }
      m4aBoxEnd(f, b3);
    }
    m4aBoxEnd(f, b2);
  }
  m4aBoxEnd(f, b);

  fclose(f);
  free(m->sizes);
  memset(m, 0, sizeof(*m));
}

/* ---- drain the (byte-aligned) super-frame bitstream into the .drm file --- */
static UINT drainSf(HANDLE_FDK_BITSTREAM hBs, FILE *f) {
  UINT bytes = FDKgetValidBits(hBs) / 8;
  if (bytes > 0) {
    fwrite(sfBuf, 1, bytes, f);
    FDKinitBitStream(hBs, sfBuf, SF_BUF_BYTES, 0, BS_WRITER);
  }
  return bytes;
}

static void usage(void) {
  printf("drm-xhe-enc — DRM xHE-AAC (USAC) encoder\n");
  printf("usage: drm-xhe-enc <input.wav> <output_prefix> [options]\n");
  printf("  -b <kbps>    bit rate, 12..128 kbit/s (default 64)\n");
  printf("  -sfms <ms>   super-frame duration 400 or 100 (default: auto)\n");
  printf("  -sbr <r>     eSBR: 0=off, 83=8:3, 21=2:1 (default), 41=4:1\n");
  printf("outputs: <prefix>.drm, <prefix>.m4a, <prefix>.cfg\n");
}

int main(int argc, char *argv[]) {
  const char *wavPath = NULL, *outPrefix = NULL;
  UINT kbps = 64;
  UINT sfMs = 0; /* 0 = auto */
  DRM_XHE_SBR_RATIO sbr = DRM_XHE_SBR_RATIO_2_1;
  char outPath[1024];

  /* ---- argument parsing (all numeric options bounds-checked) ---- */
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 10);
      if (v < 12 || v > 128) {
        printf("error: -b %ld out of range (12..128 kbit/s)\n", v);
        return 1;
      }
      kbps = (UINT)v;
    } else if (strcmp(argv[i], "-sfms") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 10);
      if (v != 400 && v != 100) {
        printf("error: -sfms must be 400 (DRM30) or 100 (DRM+)\n");
        return 1;
      }
      sfMs = (UINT)v;
    } else if (strcmp(argv[i], "-sbr") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 10);
      if (v == 0) sbr = DRM_XHE_SBR_OFF;
      else if (v == 83) sbr = DRM_XHE_SBR_RATIO_8_3;
      else if (v == 21) sbr = DRM_XHE_SBR_RATIO_2_1;
      else if (v == 41) sbr = DRM_XHE_SBR_RATIO_4_1;
      else {
        printf("error: -sbr must be 0, 83, 21 or 41\n");
        return 1;
      }
    } else if (argv[i][0] == '-') {
      usage();
      return 1;
    } else if (!wavPath) {
      wavPath = argv[i];
    } else if (!outPrefix) {
      outPrefix = argv[i];
    }
  }
  if (!wavPath || !outPrefix) {
    usage();
    return 1;
  }

  const UINT bitRate = kbps * 1000;

  /* ---- super-frame sizing: dynamic, bounded by the 12-bit directory ---- */
  if (sfMs == 0) {
    sfMs = 400;
    if ((bitRate * 400) / 8000 > TPENC_DRM_MAX_SF_BYTES) {
      sfMs = 100; /* DRM+ style frames keep high rates representable */
      printf("note: %u kbit/s x 400 ms = %u bytes exceeds the 12-bit frame-border\n"
             "      directory limit (%u); using 100 ms super frames (DRM+)\n",
             kbps, (bitRate * 400) / 8000, TPENC_DRM_MAX_SF_BYTES);
    }
  }
  const UINT sfBytes = (bitRate * sfMs) / 8000;
  if (sfBytes > TPENC_DRM_MAX_SF_BYTES || sfBytes <= TPENC_DRM_HEADER_BYTES + 2) {
    printf("error: super frame of %u bytes (%u kbit/s x %u ms / 8000) is not\n"
           "       representable in the 12-bit frame-border directory (max %u)\n",
           sfBytes, kbps, sfMs, TPENC_DRM_MAX_SF_BYTES);
    return 1;
  }

  /* ---- input ---- */
  void *wav = wav_read_open(wavPath);
  if (!wav) {
    printf("error: cannot open %s\n", wavPath);
    return 1;
  }
  int format = 0, channels = 0, sampleRate = 0, bitsPerSample = 0;
  unsigned int dataLength = 0;
  if (!wav_get_header(wav, &format, &channels, &sampleRate, &bitsPerSample, &dataLength) ||
      format != 1 || bitsPerSample != 16 || channels < 1 || channels > 2) {
    printf("error: unsupported WAV (need 16-bit PCM mono/stereo)\n");
    return 1;
  }

  /* ---- encoder ---- */
  DRM_XHE_ENC_CONFIG cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.sampleRate = (UINT)sampleRate;
  cfg.bitRate = bitRate;
  cfg.channelMode = (channels == 1) ? MODE_1 : MODE_2;
  cfg.sbrRatio = sbr;
  cfg.useDrc = 0;
  cfg.superFrameMs = sfMs;

  HANDLE_DRM_XHE_ENC hEnc = NULL;
  AACENC_ERROR err = drmXheEnc_Open(&hEnc, &cfg);
  if (err != AACENC_OK) {
    printf("error: drmXheEnc_Open -> 0x%x%s\n", err,
           (err == AACENC_INVALID_CONFIG)
               ? " (configuration rejected — bit rate / rate combination may be "
                 "outside the vendored encoder's operating range)"
               : "");
    return 1;
  }

  DRM_XHE_ENC_INFO info;
  drmXheEnc_GetInfo(hEnc, &info);
  printf("input:  %s, %d Hz, %d ch\n", wavPath, sampleRate, channels);
  printf("config: xHE-AAC (USAC) %u kbit/s %s, superFrameMs=%u\n", kbps,
         (channels == 1) ? "mono" : "stereo", sfMs);
  printf("        -> DRM super frame = %u bit/s x %u ms / 8000 = %u bytes (fixed)\n",
         bitRate, sfMs, info.superFrameBytes);

  UINT aot = ascAot(info.audioConfig, info.audioConfigBytes);
  printf("ASC:    AOT %u %s, %u bytes\n", aot, (aot == 42) ? "(USAC/xHE-AAC)" : "(UNEXPECTED)",
         info.audioConfigBytes);
  if (aot != 42) {
    printf("error: encoder config does not declare AOT 42\n");
    return 1;
  }

  /* ---- outputs ---- */
  snprintf(outPath, sizeof(outPath), "%s.cfg", outPrefix);
  {
    FILE *f = fopen(outPath, "wb");
    if (!f) { printf("error: cannot write %s\n", outPath); return 1; }
    fwrite(info.audioConfig, 1, info.audioConfigBytes, f);
    fclose(f);
  }

  snprintf(outPath, sizeof(outPath), "%s.drm", outPrefix);
  FILE *fDrm = fopen(outPath, "wb");
  if (!fDrm) { printf("error: cannot write %s\n", outPath); return 1; }

  M4A_WRITER m4a;
  snprintf(outPath, sizeof(outPath), "%s.m4a", outPrefix);
  if (m4aBegin(&m4a, outPath) != 0) {
    printf("error: cannot write %s\n", outPath);
    return 1;
  }

  DRM_SF_WRITER sfw;
  if (drmSfWriter_Init(&sfw, info.superFrameBytes) != TPENC_DRM_OK) {
    printf("error: drmSfWriter_Init(%u) failed\n", info.superFrameBytes);
    return 1;
  }
  FDK_BITSTREAM bsSf;
  FDKinitBitStream(&bsSf, sfBuf, SF_BUF_BYTES, 0, BS_WRITER);

  /* ---- encode loop: one pass, both outputs ---- */
  const UINT samplesPerFrame = info.frameLength * (UINT)channels;
  UINT frames = 0, nAus = 0, nSfTotal = 0, drmBytes = 0;

  for (;;) {
    int want = (int)(samplesPerFrame * sizeof(INT_PCM));
    int got = wav_read_data(wav, (unsigned char *)pcmBuf, want);
    if (got <= 0) break;
    if (got < want) memset((unsigned char *)pcmBuf + got, 0, want - got);

    FDK_BITSTREAM bsAu;
    FDKinitBitStream(&bsAu, auBuf, AU_BUF_BYTES, 0, BS_WRITER);
    err = drmXheEnc_EncodeFrame(hEnc, pcmBuf, samplesPerFrame, &bsAu);
    if (err != AACENC_OK) {
      printf("error: EncodeFrame(frame %u) -> 0x%x\n", frames, err);
      return 1;
    }
    frames++;

    UINT nBytes = FDKgetValidBits(&bsAu) / 8;
    if (nBytes == 0) continue; /* encoder delay priming */

    if (m4aAddSample(&m4a, auBuf, nBytes) != 0) {
      printf("error: out of memory (m4a sample table)\n");
      return 1;
    }
    UINT nSf = 0;
    TPENC_DRM_ERROR sfErr = drmSfWriter_AddAu(&sfw, auBuf, nBytes, &bsSf, &nSf);
    if (sfErr != TPENC_DRM_OK) {
      printf("error: drmSfWriter_AddAu -> %d\n", sfErr);
      return 1;
    }
    nSfTotal += nSf;
    drmBytes += drainSf(&bsSf, fDrm);
    nAus++;
  }
  wav_read_close(wav);

  UINT nSf = 0;
  drmSfWriter_Flush(&sfw, &bsSf, &nSf);
  nSfTotal += nSf;
  drmBytes += drainSf(&bsSf, fDrm);
  fclose(fDrm);

  drmXheEnc_GetInfo(hEnc, &info); /* nDelaySamples final now */
  m4aFinish(&m4a, info.audioConfig, info.audioConfigBytes, (UINT)sampleRate,
            (UINT)channels, info.frameLength, bitRate);
  drmXheEnc_Close(&hEnc);

  if (drmBytes != nSfTotal * ((bitRate * sfMs) / 8000)) {
    printf("error: super-frame stream length inconsistent\n");
    return 1;
  }

  printf("done:   %u frames -> %u AUs, delay %u samples/ch\n", frames, nAus,
         info.nDelaySamples);
  printf("wrote:  %s.drm (%u x %u-byte super frames, %u bytes)\n", outPrefix, nSfTotal,
         (bitRate * sfMs) / 8000, drmBytes);
  printf("wrote:  %s.m4a (%u samples), %s.cfg (%u bytes)\n", outPrefix, nAus, outPrefix,
         info.audioConfigBytes);

  /* ---- size audit: where every byte of the .drm stream went ---- */
  {
    UINT headerBytes = 2 * sfw.statFrames;
    UINT dirBytes = 2 * sfw.statBorders;
    UINT total = headerBytes + dirBytes + sfw.statAuBytes + sfw.statPadBytes;
    printf("audit:  %u super frames | AU payload %u B (%.2f%%) | headers %u B "
           "(%.2f%%) | directory %u B / %u entries (%.2f%%) | padding %u B (%.2f%%)\n",
           sfw.statFrames, sfw.statAuBytes, 100.0 * sfw.statAuBytes / total, headerBytes,
           100.0 * headerBytes / total, dirBytes, sfw.statBorders,
           100.0 * dirBytes / total, sfw.statPadBytes, 100.0 * sfw.statPadBytes / total);
    if (total != drmBytes) {
      printf("error: audit does not account for the stream (%u vs %u bytes)\n", total,
             drmBytes);
      return 1;
    }

    /* requested vs effective channel rate: the vendored encoder clamps to
       [8 kbit/s x channels, ccfl-dependent max], so e.g. 12 kbit/s STEREO
       becomes 16 kbit/s and the stream exceeds the 12 kbit/s channel */
    if (nAus > 0) {
      double durationSec = (double)nAus * (double)info.frameLength / (double)sampleRate;
      double effKbps = ((double)drmBytes * 8.0) / durationSec / 1000.0;
      printf("rate:   requested %u kbit/s, effective %.1f kbit/s%s\n", kbps, effKbps,
             (effKbps > (double)kbps * 1.05)
                 ? "  ** exceeds channel rate: not real-time transmittable **"
                 : "");
    }
  }
  return 0;
}
