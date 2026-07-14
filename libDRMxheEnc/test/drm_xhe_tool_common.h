/* -----------------------------------------------------------------------------
   Shared helpers for the drm-xhe-enc / drm-xhe-dec command-line tools.
   Header-only (static functions); tool code only — the library itself never
   includes this. Standard C allocation/IO is fine here (tools, not codec).
   -------------------------------------------------------------------------- */

#ifndef DRM_XHE_TOOL_COMMON_H
#define DRM_XHE_TOOL_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- big-endian file/memory helpers ---- */

static void beW32(FILE *f, unsigned int v) {
  unsigned char b[4] = {(unsigned char)(v >> 24), (unsigned char)(v >> 16),
                        (unsigned char)(v >> 8), (unsigned char)v};
  fwrite(b, 1, 4, f);
}
static void beW16(FILE *f, unsigned int v) {
  unsigned char b[2] = {(unsigned char)(v >> 8), (unsigned char)v};
  fwrite(b, 1, 2, f);
}
static unsigned int beR32(const unsigned char *p) {
  return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
         ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}

/* ---- independent CRC-8 (poly 0x1D, init 0xFF, final XOR 0xFF) ----
   Matches the DRM super-frame CRC written by tpenc_drm (FDKcrc-based);
   kept independent here so the tools cross-check the library. */
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

/* ---- streaming 16-bit PCM WAV writer (header patched on close) ---- */

typedef struct {
  FILE *f;
  unsigned int sampleRate;
  unsigned int channels;
  unsigned int dataBytes;
} WAV_WRITER;

static void wavw_writeHeader(FILE *f, unsigned int sampleRate, unsigned int channels,
                             unsigned int dataBytes) {
  unsigned int byteRate = sampleRate * channels * 2;
  unsigned int riffSize = 36 + dataBytes;
  unsigned short blockAlign = (unsigned short)(channels * 2);
  unsigned int fmtSize = 16;
  unsigned short fmtTag = 1, nCh = (unsigned short)channels, bps = 16;
  fwrite("RIFF", 1, 4, f);
  fwrite(&riffSize, 4, 1, f);
  fwrite("WAVEfmt ", 1, 8, f);
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

static int wavw_open(WAV_WRITER *w, const char *path, unsigned int sampleRate,
                     unsigned int channels) {
  memset(w, 0, sizeof(*w));
  w->f = fopen(path, "wb");
  if (!w->f) return -1;
  w->sampleRate = sampleRate;
  w->channels = channels;
  wavw_writeHeader(w->f, sampleRate, channels, 0);
  return 0;
}

static void wavw_write(WAV_WRITER *w, const void *pcm, unsigned int bytes) {
  if (w->f) {
    fwrite(pcm, 1, bytes, w->f);
    w->dataBytes += bytes;
  }
}

static void wavw_close(WAV_WRITER *w) {
  if (w->f) {
    fseek(w->f, 0, SEEK_SET);
    wavw_writeHeader(w->f, w->sampleRate, w->channels, w->dataBytes);
    fclose(w->f);
    w->f = NULL;
  }
}

/* ---- AudioSpecificConfig: parse the (escape-coded) Audio Object Type ---- */
static unsigned int ascAot(const unsigned char *asc, unsigned int len) {
  if (len < 2) return 0;
  unsigned int aot = (unsigned int)(asc[0] >> 3);
  if (aot == 31) {
    aot = 32 + ((((unsigned int)asc[0] & 0x07) << 3) | ((unsigned int)asc[1] >> 5));
  }
  return aot;
}

/* ---- whole-file loader (tools only; bounded) ---- */
static unsigned char *loadFile(const char *path, unsigned int *pLen,
                               unsigned int maxLen) {
  FILE *f = fopen(path, "rb");
  long n;
  unsigned char *buf;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0 || (unsigned long)n > maxLen) {
    fclose(f);
    return NULL;
  }
  buf = (unsigned char *)malloc((size_t)n);
  if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf);
    buf = NULL;
  }
  fclose(f);
  *pLen = (unsigned int)n;
  return buf;
}

#endif /* DRM_XHE_TOOL_COMMON_H */
