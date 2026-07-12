/* Standalone smoke test exercising aacEncOpen/SetParam/Encode/Close with
 * AOT_USAC (42) end-to-end against a synthetic sine wave, to sanity-check
 * the xHE-AAC integration beyond "it compiles". Not part of the library
 * build; compiled and run manually. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aacenc_lib.h"

int main(int argc, char **argv) {
  const int sampleRate = 24000;
  const int nChannels = 1;
  const int bitRate = 64000;
  const int nFrames = 20;

  HANDLE_AACENCODER hEnc = NULL;
  if (aacEncOpen(&hEnc, 0, nChannels) != AACENC_OK) {
    fprintf(stderr, "aacEncOpen failed\n");
    return 1;
  }

  if (aacEncoder_SetParam(hEnc, AACENC_AOT, 42) != AACENC_OK) {
    fprintf(stderr, "SetParam AOT=42 (USAC) failed\n");
    return 1;
  }
  if (aacEncoder_SetParam(hEnc, AACENC_SAMPLERATE, sampleRate) != AACENC_OK) {
    fprintf(stderr, "SetParam SAMPLERATE failed\n");
    return 1;
  }
  if (aacEncoder_SetParam(hEnc, AACENC_CHANNELMODE, MODE_1) != AACENC_OK) {
    fprintf(stderr, "SetParam CHANNELMODE failed\n");
    return 1;
  }
  if (aacEncoder_SetParam(hEnc, AACENC_BITRATE, bitRate) != AACENC_OK) {
    fprintf(stderr, "SetParam BITRATE failed\n");
    return 1;
  }
  if (argc > 1 && strcmp(argv[1], "--drm-low") == 0) {
    if (aacEncoder_SetParam(hEnc, AACENC_USAC_DRM_PROFILE, 1) != AACENC_OK) {
      fprintf(stderr, "SetParam USAC_DRM_PROFILE failed\n");
      return 1;
    }
  }

  AACENC_InfoStruct info;
  memset(&info, 0, sizeof(info));
  AACENC_ERROR initErr = aacEncEncode(hEnc, NULL, NULL, NULL, NULL);
  if (initErr != AACENC_OK) {
    fprintf(stderr, "initial aacEncEncode(NULL...) init call failed: err=0x%04x\n",
            initErr);
    return 1;
  }
  if (aacEncInfo(hEnc, &info) != AACENC_OK) {
    fprintf(stderr, "aacEncInfo failed\n");
    return 1;
  }
  printf("frameLength=%u nDelay=%u nDelayCore=%u confSize=%u\n",
         info.frameLength, info.nDelay, info.nDelayCore, info.confSize);

  int frameLength = (int)info.frameLength;
  int16_t *pcm = (int16_t *)malloc(sizeof(int16_t) * frameLength * nChannels);
  unsigned char outbuf[8192];

  int totalOutBytes = 0;
  double phase = 0.0;
  for (int f = 0; f < nFrames; f++) {
    for (int i = 0; i < frameLength * nChannels; i++) {
      pcm[i] = (int16_t)(3000.0 * sin(phase));
      phase += 2.0 * M_PI * 440.0 / sampleRate;
    }

    AACENC_BufDesc inBufDesc;
    memset(&inBufDesc, 0, sizeof(inBufDesc));
    void *inBufs[1] = {pcm};
    int inBufIds[1] = {IN_AUDIO_DATA};
    int inBufSizes[1] = {(int)(sizeof(int16_t) * frameLength * nChannels)};
    int inBufElSizes[1] = {(int)sizeof(int16_t)};
    inBufDesc.numBufs = 1;
    inBufDesc.bufs = inBufs;
    inBufDesc.bufferIdentifiers = inBufIds;
    inBufDesc.bufSizes = inBufSizes;
    inBufDesc.bufElSizes = inBufElSizes;

    AACENC_BufDesc outBufDesc;
    memset(&outBufDesc, 0, sizeof(outBufDesc));
    void *outBufs[1] = {outbuf};
    int outBufIds[1] = {OUT_BITSTREAM_DATA};
    int outBufSizes[1] = {(int)sizeof(outbuf)};
    int outBufElSizes[1] = {1};
    outBufDesc.numBufs = 1;
    outBufDesc.bufs = outBufs;
    outBufDesc.bufferIdentifiers = outBufIds;
    outBufDesc.bufSizes = outBufSizes;
    outBufDesc.bufElSizes = outBufElSizes;

    AACENC_InArgs inArgs;
    memset(&inArgs, 0, sizeof(inArgs));
    inArgs.numInSamples = frameLength * nChannels;

    AACENC_OutArgs outArgs;
    memset(&outArgs, 0, sizeof(outArgs));

    AACENC_ERROR err =
        aacEncEncode(hEnc, &inBufDesc, &outBufDesc, &inArgs, &outArgs);
    if (err != AACENC_OK) {
      fprintf(stderr, "aacEncEncode failed at frame %d: err=%d\n", f, err);
      return 1;
    }
    printf("frame %2d: numInSamples=%d numOutBytes=%d\n", f,
           outArgs.numInSamples, outArgs.numOutBytes);
    totalOutBytes += outArgs.numOutBytes;
  }

  free(pcm);
  aacEncClose(&hEnc);

  if (totalOutBytes <= 0) {
    fprintf(stderr, "FAIL: no bitstream bytes were ever produced\n");
    return 1;
  }
  printf("OK: produced %d total xHE-AAC bitstream bytes across %d frames\n",
         totalOutBytes, nFrames);
  return 0;
}
