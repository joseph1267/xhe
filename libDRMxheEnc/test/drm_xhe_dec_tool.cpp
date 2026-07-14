/* -----------------------------------------------------------------------------
   drm-xhe-dec — DRM xHE-AAC decoder CLI (fdk-aac based).

   Usage: drm-xhe-dec <input> <output.wav> [options]
     -f drm|m4a   force input format (default: auto-detect — an ISOBMFF
                  'ftyp' box or a .m4a/.mp4 extension selects the MP4 path,
                  anything else is treated as a DRM super-frame stream)
     -c <file>    AudioSpecificConfig file (REQUIRED for drm input; written
                  by drm-xhe-enc as <prefix>.cfg — stands in for the DRM SDC)
     -b <kbps>    bit rate used to derive the super-frame size (drm input)
     -sfms <ms>   super-frame duration for -b, 400 (default) or 100
     -fs <bytes>  explicit super-frame size in bytes (overrides -b/-sfms)

   MP4 path: minimal ISOBMFF parser — single audio track, 32-bit stco,
   esds ASC extraction (AOT 42 verified), stsc/stsz/stco sample mapping.
   DRM path: fixed-size frame chunking, CRC-8 verification, backwards
   directory with special previous-frame borders (see tpenc_drm.h), AU
   reassembly across straddling frames, zero-trim retry for the final
   flush-padded AU.
   -------------------------------------------------------------------------- */

#include "aacdecoder_lib.h"
#include "tpenc_drm.h" /* layout constants: TPENC_DRM_BORDER_PREV_*, MAX_BORDERS */
#include "drm_xhe_tool_common.h"

#define MAX_INPUT_BYTES (256u * 1024u * 1024u)
#define MAX_AU_BYTES (65536u)

static INT_PCM decodeOutBuf[8 * 2048];

/* ------------------------------- decoding -------------------------------- */

typedef struct {
  HANDLE_AACDECODER hDec;
  WAV_WRITER wav;
  const char *wavPath;
  unsigned int decoded;
  unsigned int failed;
  unsigned int retried;
} DEC_CTX;

static int decInit(DEC_CTX *d, const char *wavPath, unsigned char *asc,
                   unsigned int ascLen) {
  unsigned int aot = ascAot(asc, ascLen);
  printf("ASC:    %u bytes, AOT %u %s\n", ascLen, aot,
         (aot == 42) ? "(USAC/xHE-AAC)" : "(warning: not USAC)");

  memset(d, 0, sizeof(*d));
  d->wavPath = wavPath;
  d->hDec = aacDecoder_Open(TT_MP4_RAW, 1);
  if (!d->hDec) return -1;
  UCHAR *conf[1] = {asc};
  UINT confLen = ascLen;
  AAC_DECODER_ERROR e = aacDecoder_ConfigRaw(d->hDec, conf, &confLen);
  if (e != AAC_DEC_OK) {
    printf("error: decoder rejected the audio config (0x%x)\n", e);
    return -1;
  }
  return 0;
}

/* Decode one AU; on parse errors retry with 1 / 2 / all trailing zero bytes
   trimmed (DRM super-frame padding; see tpenc_drm.h). */
static void decAu(DEC_CTX *d, unsigned char *au, unsigned int len) {
  UINT tryLen[4];
  UINT nTry = 0;
  tryLen[nTry++] = len;
  if (len > 1 && au[len - 1] == 0) tryLen[nTry++] = len - 1;
  if (len > 2 && au[len - 2] == 0) tryLen[nTry++] = len - 2;
  {
    UINT z = len;
    while (z > 1 && au[z - 1] == 0) z--;
    if (z < len - 2) tryLen[nTry++] = z;
  }

  AAC_DECODER_ERROR e = AAC_DEC_UNKNOWN;
  for (UINT t = 0; t < nTry; t++) {
    UCHAR *in[1] = {au};
    UINT inLen[1] = {tryLen[t]};
    UINT valid[1] = {tryLen[t]};
    if (t > 0) aacDecoder_SetParam(d->hDec, AAC_TPDEC_CLEAR_BUFFER, 1);
    aacDecoder_Fill(d->hDec, in, inLen, valid);
    e = aacDecoder_DecodeFrame(d->hDec, decodeOutBuf,
                               sizeof(decodeOutBuf) / sizeof(INT_PCM), 0);
    if (e == AAC_DEC_OK) {
      if (t > 0) d->retried++;
      break;
    }
  }

  if (e != AAC_DEC_OK) {
    d->failed++;
    aacDecoder_SetParam(d->hDec, AAC_TPDEC_CLEAR_BUFFER, 1);
    return;
  }

  CStreamInfo *si = aacDecoder_GetStreamInfo(d->hDec);
  if (si && si->frameSize > 0 && si->numChannels > 0) {
    if (!d->wav.f) {
      if (wavw_open(&d->wav, d->wavPath, (unsigned int)si->sampleRate,
                    (unsigned int)si->numChannels) != 0) {
        printf("error: cannot write %s\n", d->wavPath);
        exit(1);
      }
      printf("output: %s, %d Hz, %d ch\n", d->wavPath, si->sampleRate, si->numChannels);
    }
    wavw_write(&d->wav, decodeOutBuf,
               (unsigned int)(si->frameSize * si->numChannels * sizeof(INT_PCM)));
  }
  d->decoded++;
}

/* ------------------------------ MP4 parsing ------------------------------ */

typedef struct {
  const unsigned char *asc;
  unsigned int ascLen;
  const unsigned char *stsz, *stco, *stsc;
} MP4_TABLES;

static unsigned int mp4DescLen(const unsigned char **pp, const unsigned char *end) {
  unsigned int len = 0;
  for (int i = 0; i < 4 && *pp < end; i++) {
    unsigned char b = *(*pp)++;
    len = (len << 7) | (b & 0x7F);
    if (!(b & 0x80)) break;
  }
  return len;
}

static void mp4ParseEsds(const unsigned char *p, const unsigned char *end,
                         MP4_TABLES *t) {
  p += 4; /* fullbox version/flags */
  if (p >= end || *p++ != 0x03) return; /* ES_Descriptor */
  mp4DescLen(&p, end);
  if (p + 3 > end) return;
  p += 2; /* ES_ID */
  unsigned char esFlags = *p++;
  if (esFlags & 0x80) p += 2;             /* dependsOn_ES_ID */
  if ((esFlags & 0x40) && p < end) p += 1 + *p; /* URL */
  if (esFlags & 0x20) p += 2;             /* OCR_ES_ID */
  if (p >= end || *p++ != 0x04) return;   /* DecoderConfigDescriptor */
  mp4DescLen(&p, end);
  p += 13; /* OTI(1) streamType(1) bufferSizeDB(3) maxBr(4) avgBr(4) */
  if (p >= end || *p++ != 0x05) return;   /* DecoderSpecificInfo = ASC */
  unsigned int dsiLen = mp4DescLen(&p, end);
  if (p + dsiLen <= end && dsiLen > 0) {
    t->asc = p;
    t->ascLen = dsiLen;
  }
}

static void mp4Walk(const unsigned char *p, const unsigned char *end, MP4_TABLES *t) {
  while (p + 8 <= end) {
    unsigned int size = beR32(p);
    const unsigned char *type = p + 4;
    if (size < 8 || p + size > end) break;
    const unsigned char *body = p + 8, *bodyEnd = p + size;

    if (!memcmp(type, "moov", 4) || !memcmp(type, "trak", 4) ||
        !memcmp(type, "mdia", 4) || !memcmp(type, "minf", 4) ||
        !memcmp(type, "stbl", 4)) {
      mp4Walk(body, bodyEnd, t);
    } else if (!memcmp(type, "stsd", 4)) {
      /* fullbox(4) + entry_count(4), then sample entries */
      const unsigned char *e = body + 8;
      if (e + 8 <= bodyEnd && !memcmp(e + 4, "mp4a", 4)) {
        /* mp4a: 8 header + 28 audio sample entry fields, then child boxes */
        mp4Walk(e + 36, e + beR32(e), t);
      }
    } else if (!memcmp(type, "esds", 4)) {
      mp4ParseEsds(body, bodyEnd, t);
    } else if (!memcmp(type, "stsz", 4)) {
      t->stsz = body;
    } else if (!memcmp(type, "stco", 4)) {
      t->stco = body;
    } else if (!memcmp(type, "stsc", 4)) {
      t->stsc = body;
    }
    p += size;
  }
}

static int decodeM4a(unsigned char *buf, unsigned int len, const char *wavPath) {
  MP4_TABLES t;
  memset(&t, 0, sizeof(t));
  mp4Walk(buf, buf + len, &t);

  if (!t.asc || !t.stsz || !t.stco || !t.stsc) {
    printf("error: MP4 parse failed (need esds/stsz/stco/stsc; co64 and\n"
           "       multi-track files are not supported by this tool)\n");
    return 1;
  }

  unsigned int uniformSize = beR32(t.stsz + 4);
  unsigned int nSamples = beR32(t.stsz + 8);
  unsigned int nChunks = beR32(t.stco + 4);
  unsigned int nStsc = beR32(t.stsc + 4);
  if (nSamples == 0 || nChunks == 0 || nStsc == 0) {
    printf("error: empty MP4 sample tables\n");
    return 1;
  }

  DEC_CTX d;
  unsigned char ascCopy[256];
  if (t.ascLen > sizeof(ascCopy)) {
    printf("error: ASC too large\n");
    return 1;
  }
  memcpy(ascCopy, t.asc, t.ascLen);
  if (decInit(&d, wavPath, ascCopy, t.ascLen) != 0) return 1;

  /* walk chunks (stsc runs) and samples */
  unsigned int sample = 0;
  for (unsigned int chunk = 1; chunk <= nChunks && sample < nSamples; chunk++) {
    /* samples per chunk: last stsc entry with first_chunk <= chunk */
    unsigned int spc = 1;
    for (unsigned int e = 0; e < nStsc; e++) {
      const unsigned char *ent = t.stsc + 8 + 12 * e;
      if (beR32(ent) <= chunk) spc = beR32(ent + 4);
    }
    unsigned int off = beR32(t.stco + 8 + 4 * (chunk - 1));
    for (unsigned int k = 0; k < spc && sample < nSamples; k++, sample++) {
      unsigned int sz = uniformSize ? uniformSize : beR32(t.stsz + 12 + 4 * sample);
      if (off + sz > len || sz == 0 || sz > MAX_AU_BYTES) {
        printf("error: sample %u out of bounds\n", sample);
        return 1;
      }
      decAu(&d, buf + off, sz);
      off += sz;
    }
  }

  wavw_close(&d.wav);
  printf("decode: %u/%u samples OK (%u failed, %u via zero-trim retry)\n", d.decoded,
         nSamples, d.failed, d.retried);
  return (d.decoded > 0 && d.failed == 0) ? 0 : (d.decoded > 0 ? 2 : 1);
}

/* --------------------------- DRM de-framing ------------------------------ */

/*
 * Determine the frame-border count of a super frame. It cannot be read
 * blindly from the last directory entry: a frame carrying only the
 * continuation of a straddling AU has ZERO borders and no directory, so the
 * last two bytes are payload. Disambiguation uses redundancy already in the
 * stream: for the true k, (a) every directory entry repeats k in its 4-bit
 * count field, and (b) the header CRC-8 (over reserved byte + directory)
 * matches — including k = 0, which validates against the constant CRC over
 * the lone reserved byte. Returns -1 if no candidate is consistent.
 */
static int drmFindBorderCount(const unsigned char *sf, unsigned int L) {
  if (sf[1] != 0x00) return -1;
  /* Scan DESCENDING: the true k always passes all checks, while a candidate
     larger than the true k would need every fake payload-byte entry to
     repeat its count AND satisfy position monotonicity AND match the CRC —
     vanishing probability. Ascending order would let k=0 (whose CRC is a
     constant) falsely win ~1/256 of frames. */
  for (int k = TPENC_DRM_MAX_BORDERS; k >= 0; k--) {
    if (2 + 2 * (unsigned int)k >= L) continue;
    unsigned int payloadEnd = L - 2 * (unsigned int)k;
    int ok = 1;
    unsigned int prevPos = 0;
    for (int i = 0; i < k && ok; i++) {
      const unsigned char *e = &sf[L - 2 * (i + 1)];
      unsigned int pos = ((unsigned int)e[0] << 4) | ((unsigned int)e[1] >> 4);
      if ((e[1] & 0x0F) != (unsigned int)k) ok = 0;               /* count field */
      else if (i == 0 && pos >= TPENC_DRM_BORDER_PREV_2) continue; /* special */
      else if (pos < 2 || pos >= payloadEnd || pos <= prevPos) ok = 0;
      else prevPos = pos;
    }
    if (!ok) continue;
    unsigned char ctrl[1 + 2 * TPENC_DRM_MAX_BORDERS];
    ctrl[0] = sf[1];
    memcpy(&ctrl[1], &sf[L - 2 * k], 2 * (unsigned int)k);
    if (crc8_drm(ctrl, 1 + 2 * (unsigned int)k) == sf[0]) {
      return k;
    }
  }
  return -1;
}

static int decodeDrm(unsigned char *buf, unsigned int len, const char *wavPath,
                     unsigned char *asc, unsigned int ascLen, unsigned int sfBytes) {
  if (sfBytes <= TPENC_DRM_HEADER_BYTES + 2 || sfBytes > TPENC_DRM_MAX_SF_BYTES) {
    printf("error: super-frame size %u out of range (%u..%u)\n", sfBytes,
           TPENC_DRM_HEADER_BYTES + 3, TPENC_DRM_MAX_SF_BYTES);
    return 1;
  }
  unsigned int nSf = len / sfBytes;
  if (nSf == 0) {
    printf("error: input shorter than one super frame\n");
    return 1;
  }
  if (len % sfBytes != 0) {
    printf("warning: %u trailing bytes ignored (stream not a multiple of %u)\n",
           len % sfBytes, sfBytes);
  }
  printf("stream: %u super frames of %u bytes\n", nSf, sfBytes);

  DEC_CTX d;
  if (decInit(&d, wavPath, asc, ascLen) != 0) return 1;

  unsigned char *reAu = (unsigned char *)malloc(MAX_AU_BYTES);
  unsigned int reAuLen = 0, crcFailures = 0, nAus = 0;
  int started = 0, overflow = 0;

  for (unsigned int s = 0; s < nSf; s++) {
    const unsigned char *sf = &buf[s * sfBytes];
    unsigned int L = sfBytes;
    int bc = drmFindBorderCount(sf, L);
    if (bc < 0) {
      /* no CRC-consistent border count: corrupt frame; drop the AU under
         reassembly and resync at the next frame's first border */
      crcFailures++;
      reAuLen = 0;
      started = 0;
      continue;
    }
    unsigned int borderCount = (unsigned int)bc;
    unsigned int payloadBytes = L - 2 - 2 * borderCount;

    unsigned int border[TPENC_DRM_MAX_BORDERS];
    for (unsigned int i = 0; i < borderCount; i++) {
      const unsigned char *e = &sf[L - 2 * (i + 1)];
      border[i] = ((unsigned int)e[0] << 4) | ((unsigned int)e[1] >> 4);
    }

    unsigned int bStart = 0;
    if (borderCount > 0 && border[0] >= TPENC_DRM_BORDER_PREV_2) {
      /* AU started in the last 1-2 bytes of the previous super frame */
      unsigned int k = (border[0] == TPENC_DRM_BORDER_PREV_1) ? 1 : 2;
      if (started && reAuLen >= k) {
        unsigned char tail[2];
        memcpy(tail, reAu + reAuLen - k, k);
        reAuLen -= k;
        decAu(&d, reAu, reAuLen);
        nAus++;
        memcpy(reAu, tail, k);
        reAuLen = k;
      }
      started = 1;
      bStart = 1;
    }

    unsigned int cursor = 2;
    for (unsigned int i = bStart; i <= borderCount; i++) {
      unsigned int segEnd = (i < borderCount) ? border[i] : (2 + payloadBytes);
      if (segEnd > 2 + payloadBytes || (i < borderCount && segEnd < 2)) {
        crcFailures++; /* corrupt border index */
        break;
      }
      if (segEnd > cursor && started) {
        unsigned int n = segEnd - cursor;
        if (reAuLen + n > MAX_AU_BYTES) {
          overflow = 1;
          reAuLen = 0; /* drop the runaway AU, resync at next border */
        } else {
          memcpy(reAu + reAuLen, &sf[cursor], n);
          reAuLen += n;
        }
      }
      if (i < borderCount) {
        if (started) {
          decAu(&d, reAu, reAuLen);
          nAus++;
        }
        started = 1;
        reAuLen = 0;
        cursor = segEnd;
      } else {
        cursor = segEnd;
      }
    }
  }
  if (started && reAuLen > 0) { /* final AU (may carry flush padding) */
    decAu(&d, reAu, reAuLen);
    nAus++;
  }
  free(reAu);

  wavw_close(&d.wav);
  printf("decode: %u AUs, %u OK (%u failed, %u via zero-trim retry), crcFailures=%u%s\n",
         nAus, d.decoded, d.failed, d.retried, crcFailures,
         overflow ? ", AU OVERFLOW (wrong -fs/-b?)" : "");
  return (d.decoded > 0 && d.failed <= 1 && crcFailures == 0 && !overflow)
             ? 0
             : (d.decoded > 0 ? 2 : 1);
}

/* --------------------------------- main ---------------------------------- */

static void usage(void) {
  printf("drm-xhe-dec — DRM xHE-AAC decoder (fdk-aac)\n");
  printf("usage: drm-xhe-dec <input> <output.wav> [options]\n");
  printf("  -f drm|m4a   force input format (default: auto-detect)\n");
  printf("  -c <file>    AudioSpecificConfig (.cfg) — required for drm input\n");
  printf("  -b <kbps>    bit rate to derive the super-frame size (drm input)\n");
  printf("  -sfms <ms>   super-frame duration for -b: 400 (default) or 100\n");
  printf("  -fs <bytes>  explicit super-frame size (overrides -b/-sfms)\n");
}

int main(int argc, char *argv[]) {
  const char *inPath = NULL, *wavPath = NULL, *cfgPath = NULL, *force = NULL;
  unsigned int kbps = 0, sfMs = 400, sfBytes = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
      force = argv[++i];
      if (strcmp(force, "drm") != 0 && strcmp(force, "m4a") != 0) {
        printf("error: -f must be 'drm' or 'm4a'\n");
        return 1;
      }
    } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      cfgPath = argv[++i];
    } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 10);
      if (v < 12 || v > 128) {
        printf("error: -b %ld out of range (12..128 kbit/s)\n", v);
        return 1;
      }
      kbps = (unsigned int)v;
    } else if (strcmp(argv[i], "-sfms") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 10);
      if (v != 400 && v != 100) {
        printf("error: -sfms must be 400 or 100\n");
        return 1;
      }
      sfMs = (unsigned int)v;
    } else if (strcmp(argv[i], "-fs") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 10);
      if (v <= 0 || v > TPENC_DRM_MAX_SF_BYTES) {
        printf("error: -fs %ld out of range (1..%u)\n", v, TPENC_DRM_MAX_SF_BYTES);
        return 1;
      }
      sfBytes = (unsigned int)v;
    } else if (argv[i][0] == '-') {
      usage();
      return 1;
    } else if (!inPath) {
      inPath = argv[i];
    } else if (!wavPath) {
      wavPath = argv[i];
    }
  }
  if (!inPath || !wavPath) {
    usage();
    return 1;
  }

  unsigned int len = 0;
  unsigned char *buf = loadFile(inPath, &len, MAX_INPUT_BYTES);
  if (!buf || len < 16) {
    printf("error: cannot read %s (or larger than %u MiB)\n", inPath,
           MAX_INPUT_BYTES >> 20);
    return 1;
  }

  /* ---- format auto-detection ---- */
  int isM4a;
  if (force) {
    isM4a = (strcmp(force, "m4a") == 0);
    printf("format: %s (forced)\n", isM4a ? "m4a" : "drm");
  } else {
    size_t n = strlen(inPath);
    int extM4a = (n > 4 && (!strcmp(inPath + n - 4, ".m4a") || !strcmp(inPath + n - 4, ".mp4")));
    int ftyp = (len >= 12 && memcmp(buf + 4, "ftyp", 4) == 0);
    isM4a = (ftyp || extM4a);
    printf("format: %s (auto: %s)\n", isM4a ? "m4a" : "drm",
           ftyp ? "ftyp box found" : (extM4a ? "extension" : "no ISOBMFF signature"));
  }

  if (isM4a) {
    return decodeM4a(buf, len, wavPath);
  }

  /* ---- DRM path: needs config + frame size ---- */
  if (!cfgPath) {
    printf("error: drm input requires -c <file.cfg> (the AudioSpecificConfig\n"
           "       written by drm-xhe-enc; stands in for the DRM SDC)\n");
    return 1;
  }
  unsigned int ascLen = 0;
  unsigned char *asc = loadFile(cfgPath, &ascLen, 4096);
  if (!asc || ascLen == 0) {
    printf("error: cannot read config %s\n", cfgPath);
    return 1;
  }
  if (sfBytes == 0) {
    if (kbps == 0) {
      printf("error: drm input needs the frame size: pass -fs <bytes>, or\n"
             "       -b <kbps> (with optional -sfms 400|100)\n");
      return 1;
    }
    sfBytes = (kbps * 1000 * sfMs) / 8000;
    printf("frame:  %u kbit/s x %u ms / 8000 = %u bytes\n", kbps, sfMs, sfBytes);
  }

  return decodeDrm(buf, len, wavPath, asc, ascLen, sfBytes);
}
