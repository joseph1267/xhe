/* -----------------------------------------------------------------------------
   DRM xHE-AAC round-trip test:
     WAV PCM -> drmXheEnc (vendored libxaac engine, FDK memory)
             -> access units in HANDLE_FDK_BITSTREAM
             -> DRM audio super frames (tpenc_drm)
             -> harness de-framer (mirrors tpenc_drm's documented layout,
                with an INDEPENDENT bit-by-bit CRC-8 implementation)
             -> byte-exact AU sequence comparison
     plus an experimental decode attempt of the raw AUs with fdk-aac's own
     decoder (TT_MP4_RAW + the config captured at encoder create time).

   Exit code 0 iff the AU sequence survives framing/de-framing byte-exact.
   -------------------------------------------------------------------------- */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drm_xhe_enc.h"
#include "tpenc_drm.h"
#include "FDK_bitstream.h"
#include "aacdecoder_lib.h"

extern "C" {
#include "wavreader.h"
}

#define MAX_TEST_FRAMES (16384)       /* hard cap; default is the whole file */
#define AU_BUF_BYTES (32768)          /* power of 2 for FDK bit buffer */
#define SF_STREAM_BYTES (1 << 22)     /* 4 MiB, power of 2 */
#define MAX_AUS (MAX_TEST_FRAMES + 8) /* encoded AUs (incl. flush margin) */

static UCHAR auBuf[AU_BUF_BYTES];
static UCHAR sfStream[SF_STREAM_BYTES];
static UCHAR *auData[MAX_AUS];
static UINT auLen[MAX_AUS];
static UCHAR *reAuData[MAX_AUS]; /* de-framed (receiver-side) AUs */
static UINT reAuLenArr[MAX_AUS];
static INT_PCM pcmBuf[8192];
static INT_PCM decodeOutBuf[8 * 2048];

/* Compare a de-framed AU with the original.
   Returns 0: byte-exact; 1: original + all-zero tail (structural super-frame
   padding: 1-2 byte end-of-frame slack too small for a directory entry, the
   15-border cap, or the end-of-stream flush — AU ends are implicit in the
   DRM layout, so frame padding adheres to the frame's last AU; USAC AUs are
   self-terminating, so decoders ignore it); -1: content mismatch. */
static int checkAu(const UCHAR *re, UINT reN, const UCHAR *orig, UINT origN) {
  if (reN < origN || memcmp(re, orig, origN) != 0) return -1;
  if (reN == origN) return 0;
  for (UINT i = origN; i < reN; i++) {
    if (re[i] != 0) return -1;
  }
  return 1;
}

/* Minimal 16-bit PCM WAV writer: placeholder header, patched on close. */
static void wavw_header(FILE *f, unsigned int sampleRate, unsigned int channels,
                        unsigned int dataBytes) {
  unsigned int byteRate = sampleRate * channels * 2;
  unsigned int riffSize = 36 + dataBytes;
  unsigned short blockAlign = (unsigned short)(channels * 2);
  fwrite("RIFF", 1, 4, f);
  fwrite(&riffSize, 4, 1, f);
  fwrite("WAVEfmt ", 1, 8, f);
  unsigned int fmtSize = 16;
  unsigned short fmtTag = 1, nCh = (unsigned short)channels, bps = 16;
  fwrite(&fmtSize, 4, 1, f);
  fwrite(&fmtTag, 2, 1, f);
  fwrite(&nCh, 2, 1, f);
  fwrite(&sampleRate, 4, 1, f);
  fwrite(&byteRate, 4, 1, f);
  fwrite(&blockAlign, 2, 1, f);
  fwrite(&bps, 2, 1, f);
  fwrite("data", 1, 4, f);
  fwrite(&dataBytes, 4, 1, f);
}

/* Independent CRC-8 (poly 0x1D, init 0xFF, final XOR 0xFF) — deliberately
   NOT FDKcrc, to cross-check the encoder's FDKcrc usage. */
static unsigned char crc8_drm(const unsigned char *data, unsigned int len) {
  unsigned char crc = 0xFF;
  for (unsigned int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (unsigned char)((crc << 1) ^ 0x1D) : (unsigned char)(crc << 1);
    }
  }
  return crc ^ 0xFF;
}

int main(int argc, char *argv[]) {
  const char *wavPath = (argc > 1) ? argv[1] : "melo_ost_48k.wav";
  const char *outPrefix = (argc > 2) ? argv[2] : "drm_xhe_out";
  UINT maxFrames = (argc > 3) ? (UINT)atoi(argv[3]) : 0; /* 0 = whole file */
  const UINT bitRate = 64000;
  const UINT superFrameMs = 400;
  char outPath[1024];

  if (maxFrames == 0 || maxFrames > MAX_TEST_FRAMES) {
    maxFrames = MAX_TEST_FRAMES;
  }

  /* ---------------- read WAV header ---------------- */
  void *wav = wav_read_open(wavPath);
  if (!wav) {
    printf("FAIL: cannot open %s\n", wavPath);
    return 1;
  }
  int format = 0, channels = 0, sampleRate = 0, bitsPerSample = 0;
  unsigned int dataLength = 0;
  if (!wav_get_header(wav, &format, &channels, &sampleRate, &bitsPerSample, &dataLength) ||
      format != 1 || bitsPerSample != 16 || channels < 1 || channels > 2) {
    printf("FAIL: unsupported WAV (need 16-bit PCM mono/stereo)\n");
    return 1;
  }
  printf("input: %s, %d Hz, %d ch, 16 bit\n", wavPath, sampleRate, channels);

  /* ---------------- open encoder ---------------- */
  DRM_XHE_ENC_CONFIG cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.sampleRate = (UINT)sampleRate;
  cfg.bitRate = bitRate;
  cfg.channelMode = (channels == 1) ? MODE_1 : MODE_2;
  cfg.sbrRatio = DRM_XHE_SBR_RATIO_2_1;
  cfg.useDrc = 0;
  cfg.superFrameMs = superFrameMs;

  HANDLE_DRM_XHE_ENC hEnc = NULL;
  AACENC_ERROR encErr = drmXheEnc_Open(&hEnc, &cfg);
  if (encErr != AACENC_OK) {
    printf("FAIL: drmXheEnc_Open -> 0x%x\n", encErr);
    return 1;
  }

  DRM_XHE_ENC_INFO info;
  drmXheEnc_GetInfo(hEnc, &info);
  printf("encoder: frameLength=%u samples/ch, superFrame=%u bytes, config=%u bytes\n",
         info.frameLength, info.superFrameBytes, info.audioConfigBytes);

  /* ---------------- super-frame writer ---------------- */
  DRM_SF_WRITER sfw;
  if (drmSfWriter_Init(&sfw, info.superFrameBytes) != TPENC_DRM_OK) {
    printf("FAIL: drmSfWriter_Init (superFrameBytes=%u)\n", info.superFrameBytes);
    return 1;
  }
  FDK_BITSTREAM bsSf;
  FDKinitBitStream(&bsSf, sfStream, SF_STREAM_BYTES, 0, BS_WRITER);

  /* ---------------- encode + frame loop ---------------- */
  const UINT samplesPerFrame = info.frameLength * (UINT)channels;
  UINT nAus = 0, nSfTotal = 0, frames = 0;

  while (frames < maxFrames) {
    int bytesWanted = (int)(samplesPerFrame * sizeof(INT_PCM));
    int bytesRead = wav_read_data(wav, (unsigned char *)pcmBuf, bytesWanted);
    if (bytesRead <= 0) break;
    if (bytesRead < bytesWanted) {
      memset((unsigned char *)pcmBuf + bytesRead, 0, bytesWanted - bytesRead);
    }

    FDK_BITSTREAM bsAu;
    FDKinitBitStream(&bsAu, auBuf, AU_BUF_BYTES, 0, BS_WRITER);

    encErr = drmXheEnc_EncodeFrame(hEnc, pcmBuf, samplesPerFrame, &bsAu);
    if (encErr != AACENC_OK) {
      printf("FAIL: drmXheEnc_EncodeFrame(frame %u) -> 0x%x\n", frames, encErr);
      return 1;
    }

    UINT nBytes = FDKgetValidBits(&bsAu) / 8;
    if (nBytes > 0 && nAus < MAX_AUS) {
      auData[nAus] = (UCHAR *)malloc(nBytes); /* harness-only bookkeeping */
      memcpy(auData[nAus], auBuf, nBytes);
      auLen[nAus] = nBytes;
      nAus++;

      UINT nSf = 0;
      TPENC_DRM_ERROR sfErr = drmSfWriter_AddAu(&sfw, auBuf, nBytes, &bsSf, &nSf);
      if (sfErr != TPENC_DRM_OK) {
        printf("FAIL: drmSfWriter_AddAu -> %d\n", sfErr);
        return 1;
      }
      nSfTotal += nSf;
    }
    frames++;
  }
  wav_read_close(wav);

  UINT nSf = 0;
  drmSfWriter_Flush(&sfw, &bsSf, &nSf);
  nSfTotal += nSf;
  FDKsyncCache(&bsSf);

  UINT sfBytesTotal = FDKgetValidBits(&bsSf) / 8;
  drmXheEnc_GetInfo(hEnc, &info); /* re-query: nDelaySamples final now */
  printf("encoded: %u frames -> %u AUs -> %u super frames (%u bytes), delay=%u samples/ch\n",
         frames, nAus, nSfTotal, sfBytesTotal, info.nDelaySamples);

  if (nAus == 0 || sfBytesTotal != nSfTotal * info.superFrameBytes) {
    printf("FAIL: super-frame stream length mismatch\n");
    return 1;
  }

  /* dump the encoded DRM super-frame stream */
  snprintf(outPath, sizeof(outPath), "%s.drm", outPrefix);
  {
    FILE *f = fopen(outPath, "wb");
    if (f) {
      fwrite(sfStream, 1, sfBytesTotal, f);
      fclose(f);
      printf("wrote: %s (%u bytes, %u super frames of %u bytes)\n", outPath, sfBytesTotal,
             nSfTotal, info.superFrameBytes);
    }
  }

  /* dump the SDC audio-config source (ASC captured at create time), as
     consumed by the external drm-xhe-dec verifier */
  snprintf(outPath, sizeof(outPath), "%s.cfg", outPrefix);
  {
    FILE *f = fopen(outPath, "wb");
    if (f) {
      fwrite(info.audioConfig, 1, info.audioConfigBytes, f);
      fclose(f);
      printf("wrote: %s (%u bytes)\n", outPath, info.audioConfigBytes);
    }
  }

  /* ---------------- de-frame (mirrors tpenc_drm layout) ---------------- */
  const UINT L = info.superFrameBytes;
  UCHAR *reAu = (UCHAR *)malloc(SF_STREAM_BYTES); /* current AU accumulator */
  UINT reAuLen = 0, reAuIdx = 0, crcFailures = 0, auMismatches = 0, paddedTails = 0;
  int started = 0;

  for (UINT s = 0; s < nSfTotal; s++) {
    const UCHAR *sf = &sfStream[s * L];
    /* layout: [crc_8][reserved 0x00][payload][backwards directory];
       border count recovered from the 4-bit field of the LAST entry
       (border 0's entry occupies the final 2 bytes of the frame) */
    UINT borderCount = (UINT)(sf[L - 1] & 0x0F);
    UINT payloadBytes = L - 2 - 2 * borderCount;
    /* CRC-8 covers reserved byte + directory (control info), in stream
       order; independently recomputed (not FDKcrc) */
    {
      UCHAR ctrl[2 + 2 * TPENC_DRM_MAX_BORDERS];
      ctrl[0] = sf[1];
      memcpy(&ctrl[1], &sf[2 + payloadBytes], 2 * borderCount);
      if (crc8_drm(ctrl, 1 + 2 * borderCount) != sf[0] || sf[1] != 0x00) {
        crcFailures++;
      }
    }
    UINT border[TPENC_DRM_MAX_BORDERS];
    for (UINT i = 0; i < borderCount; i++) {
      const UCHAR *e = &sf[L - 2 * (i + 1)]; /* backwards directory */
      border[i] = ((UINT)e[0] << 4) | ((UINT)e[1] >> 4); /* 12-bit index */
    }

    /* close the current AU: compare against the original, store for the
       receiver-side decode stage */
    auto closeAu = [&](UINT sfIdx) {
      if (reAuIdx >= MAX_AUS) return;
      int c = (reAuIdx < nAus) ? checkAu(reAu, reAuLen, auData[reAuIdx], auLen[reAuIdx]) : -1;
      if (c < 0) {
        auMismatches++;
        printf("  mismatch: AU %u in SF %u (%u bytes reconstructed)\n", reAuIdx, sfIdx,
               reAuLen);
      } else if (c > 0) {
        paddedTails++;
      }
      reAuData[reAuIdx] = (UCHAR *)malloc(reAuLen ? reAuLen : 1);
      memcpy(reAuData[reAuIdx], reAu, reAuLen);
      reAuLenArr[reAuIdx] = reAuLen;
      reAuIdx++;
    };

    UINT bStart = 0;
    if (borderCount > 0 && border[0] >= TPENC_DRM_BORDER_PREV_2) {
      /* special border: the new AU started in the last 1-2 bytes of the
         PREVIOUS super frame — carve those bytes off the accumulated AU */
      UINT k = (border[0] == TPENC_DRM_BORDER_PREV_1) ? 1 : 2;
      if (started && reAuLen >= k) {
        UCHAR tail[2];
        memcpy(tail, reAu + reAuLen - k, k);
        reAuLen -= k;
        closeAu(s);
        memcpy(reAu, tail, k);
        reAuLen = k;
      }
      started = 1;
      bStart = 1; /* border[0] carries no position in this frame */
    }

    UINT cursor = 2;
    for (UINT i = bStart; i <= borderCount; i++) {
      UINT segEnd = (i < borderCount) ? border[i] : (2 + payloadBytes);
      if (segEnd > cursor && started) {
        memcpy(reAu + reAuLen, &sf[cursor], segEnd - cursor);
        reAuLen += segEnd - cursor;
      }
      if (i < borderCount) {
        /* a border: close the current AU, start the next */
        if (started) {
          closeAu(s);
        }
        started = 1;
        reAuLen = 0;
        cursor = border[i];
      } else {
        cursor = segEnd;
      }
    }
  }
  /* final AU: closed by end of stream; flush zero-padding may trail it */
  if (started && reAuIdx < nAus) {
    int c = checkAu(reAu, reAuLen, auData[reAuIdx], auLen[reAuIdx]);
    if (c < 0) {
      auMismatches++;
    } else if (c > 0) {
      paddedTails++;
    }
    reAuData[reAuIdx] = (UCHAR *)malloc(reAuLen ? reAuLen : 1);
    memcpy(reAuData[reAuIdx], reAu, reAuLen);
    reAuLenArr[reAuIdx] = reAuLen;
    reAuIdx++;
  }

  printf("de-framed: %u AUs, crcFailures=%u, mismatches=%u, paddedTails=%u\n", reAuIdx,
         crcFailures, auMismatches, paddedTails);

  /* ------- decode the DE-FRAMED AUs (receiver-side data) with fdk-aac ------- */
  UINT decoded = 0;
  {
    HANDLE_AACDECODER hDec = aacDecoder_Open(TT_MP4_RAW, 1);
    UCHAR *conf[1] = {info.audioConfig};
    UINT confLen = info.audioConfigBytes;
    AAC_DECODER_ERROR dErr = aacDecoder_ConfigRaw(hDec, conf, &confLen);
    if (dErr != AAC_DEC_OK) {
      printf("decode: ConfigRaw -> 0x%x (config format needs SDC follow-up)\n", dErr);
    } else {
      UINT pcmBytesOut = 0;
      FILE *fDec = NULL;
      snprintf(outPath, sizeof(outPath), "%s_decoded.wav", outPrefix);
      UINT trimmedRetries = 0;
      for (UINT i = 0; i < reAuIdx; i++) {
        /* Attempt lengths: full AU, then with 1 / 2 trailing zero bytes
           trimmed (structural super-frame slack), then with the whole zero
           tail trimmed (end-of-stream flush padding). A retry only happens
           on a parse error, so AUs that legitimately end in zeros and parse
           at full length are never touched. */
        UINT tryLen[4];
        UINT nTry = 0;
        UINT full = reAuLenArr[i];
        tryLen[nTry++] = full;
        if (full > 1 && reAuData[i][full - 1] == 0) tryLen[nTry++] = full - 1;
        if (full > 2 && reAuData[i][full - 2] == 0) tryLen[nTry++] = full - 2;
        {
          UINT z = full;
          while (z > 1 && reAuData[i][z - 1] == 0) z--;
          if (z < full - 2) tryLen[nTry++] = z;
        }

        dErr = AAC_DEC_UNKNOWN;
        for (UINT t = 0; t < nTry; t++) {
          UCHAR *in[1] = {reAuData[i]};
          UINT inLen[1] = {tryLen[t]};
          UINT valid[1] = {tryLen[t]};
          if (t > 0) {
            /* discard leftover bytes of the failed attempt */
            aacDecoder_SetParam(hDec, AAC_TPDEC_CLEAR_BUFFER, 1);
          }
          aacDecoder_Fill(hDec, in, inLen, valid);
          dErr = aacDecoder_DecodeFrame(hDec, decodeOutBuf,
                                        sizeof(decodeOutBuf) / sizeof(INT_PCM), 0);
          if (dErr == AAC_DEC_OK) {
            if (t > 0) trimmedRetries++;
            break;
          }
        }
        if (dErr != AAC_DEC_OK) {
          printf("  decode error at AU %u: 0x%x (auLen=%u)\n", i, dErr, reAuLenArr[i]);
          aacDecoder_SetParam(hDec, AAC_TPDEC_CLEAR_BUFFER, 1);
        }
        if (dErr == AAC_DEC_OK) {
          decoded++;
          CStreamInfo *si = aacDecoder_GetStreamInfo(hDec);
          if (si != NULL && si->frameSize > 0 && si->numChannels > 0) {
            if (fDec == NULL) {
              fDec = fopen(outPath, "wb");
              if (fDec) {
                wavw_header(fDec, (unsigned int)si->sampleRate,
                            (unsigned int)si->numChannels, 0); /* patched below */
              }
            }
            if (fDec) {
              UINT n = (UINT)(si->frameSize * si->numChannels * sizeof(INT_PCM));
              fwrite(decodeOutBuf, 1, n, fDec);
              pcmBytesOut += n;
            }
          }
        }
      }
      printf("decode: %u/%u de-framed AUs decoded OK by fdk-aac (TT_MP4_RAW), "
             "%u via zero-trim retry\n",
             decoded, reAuIdx, trimmedRetries);
      if (fDec) {
        CStreamInfo *si = aacDecoder_GetStreamInfo(hDec);
        fseek(fDec, 0, SEEK_SET);
        wavw_header(fDec, (unsigned int)si->sampleRate, (unsigned int)si->numChannels,
                    pcmBytesOut);
        fclose(fDec);
        printf("wrote: %s (%u PCM bytes, %d Hz, %d ch)\n", outPath, pcmBytesOut,
               si->sampleRate, si->numChannels);
      }
    }
    aacDecoder_Close(hDec);
  }

  /* PASS criteria: framing is content-lossless (CRC + byte-exact prefixes)
     and every de-framed AU decodes on the receiver side, except that the
     FINAL AU may fail: end-of-stream flush padding adheres to it (real
     broadcast streams do not end, so this is a test-only artifact). */
  int pass = (reAuIdx == nAus) && (crcFailures == 0) && (auMismatches == 0) &&
             (decoded + 1 >= reAuIdx);
  if (paddedTails > 0) {
    printf("note: %u padded-tail AU(s) — structural super-frame slack, see tpenc_drm.h\n",
           paddedTails);
  }
  printf(pass ? "ROUNDTRIP: PASS\n" : "ROUNDTRIP: FAIL\n");
  return pass ? 0 : 1;
}
