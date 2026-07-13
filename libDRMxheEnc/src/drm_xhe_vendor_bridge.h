/* -----------------------------------------------------------------------------
   DRM xHE-AAC encoder library (libDRMxheEnc) — vendor bridge interface

   NEUTRAL header (plain C types only, no fdk-aac or vendored includes; see
   drm_xhe_enc_pool.h for why). Declares the vendor-side engine operations
   implemented in drm_xhe_vendor_bridge.cpp and consumed by the FDK-side
   public wrapper drm_xhe_enc.cpp.

   All int return values are raw libxaac IA_ERRORCODEs (fatal bit 0x80000000);
   the FDK side maps them to AACENC_ERROR via DrmXheBridge_ClassifyError().
   -------------------------------------------------------------------------- */

#ifndef DRM_XHE_VENDOR_BRIDGE_H
#define DRM_XHE_VENDOR_BRIDGE_H

#include "drm_xhe_enc_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create-time parameters, already validated FDK-side (mono/stereo gate). */
typedef struct {
  unsigned int sampleRate; /* input PCM rate in Hz */
  unsigned int bitRate;    /* bit/s */
  unsigned int nChannels;  /* 1 or 2 */
  int ccflIdx;             /* libxaac core-coder framelength index:
                              1 = ccfl 1024 no eSBR, 2 = 768 + 8:3,
                              3 = 1024 + 2:1,        4 = 1024 + 4:1 */
  int useDrc;              /* 1: embed MPEG-D DRC/loudness metadata */
} DRMXHE_BRIDGE_CONFIG;

/* Post-create facts read back from the vendored encoder. */
typedef struct {
  unsigned int inputSizeBytes;  /* PCM bytes consumed per EncodeFrame call */
  unsigned int pcmWordSizeBits; /* 16 */
  unsigned int coreSampleRate;  /* vendored samp_freq (before SBR ratio) */
  float downSamplingRatio;
  unsigned int configBytes;     /* raw-ES config header emitted at create
                                   (copied to pConfigOut, clipped to
                                   configOutMax; configBytes is the full
                                   length regardless) */
} DRMXHE_BRIDGE_INFO;

/* Error classes for FDK-side AACENC_ERROR mapping. */
typedef enum {
  DRMXHE_ERRCLASS_OK = 0,      /* IA_NO_ERROR or non-fatal */
  DRMXHE_ERRCLASS_MEM = 1,     /* allocation / pool bound violation */
  DRMXHE_ERRCLASS_CONFIG = 2,  /* invalid or unsupported configuration */
  DRMXHE_ERRCLASS_FATAL = 3    /* any other fatal engine error */
} DRMXHE_ERRCLASS;

/*
 * Fill the (opaque-storage-backed) vendored config structs, install the pool
 * dispenser hooks with pv_mem_ctx = pPool, and run ixheaace_create().
 * On fatal error the vendored cleanup path has already run through the
 * (no-op) free hook; the caller frees the FDK blocks.
 */
int DrmXheBridge_Create(void *pInCfgMem, unsigned int inCfgMemSize,
                        void *pOutCfgMem, unsigned int outCfgMemSize,
                        void *pDrcCfgMem, unsigned int drcCfgMemSize,
                        DRMXHE_MEM_POOL *pPool, const DRMXHE_BRIDGE_CONFIG *pCfg,
                        DRMXHE_BRIDGE_INFO *pInfo, unsigned char *pConfigOut,
                        unsigned int configOutMax);

/*
 * Encode one access unit: copies pcmBytes (zero-padding up to inputSizeBytes,
 * mirroring the vendored testbench's final-frame behavior) into the INPUT
 * region, runs ixheaace_process(), and returns a pointer into the OUTPUT
 * region (valid until the next bridge call on this instance).
 */
int DrmXheBridge_EncodeFrame(void *pInCfgMem, void *pOutCfgMem,
                             const unsigned char *pPcm, unsigned int pcmBytes,
                             const unsigned char **ppAuData,
                             unsigned int *pAuBytes);

/* Run ixheaace_delete() (frees flow through the no-op pool hook). */
int DrmXheBridge_Delete(void *pOutCfgMem);

/* Classify a raw IA_ERRORCODE for AACENC_ERROR mapping. */
DRMXHE_ERRCLASS DrmXheBridge_ClassifyError(int err);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DRM_XHE_VENDOR_BRIDGE_H */
