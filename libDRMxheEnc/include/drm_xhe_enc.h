/* -----------------------------------------------------------------------------
   DRM xHE-AAC encoder library (libDRMxheEnc)

   Public API for the xHE-AAC (MPEG-D USAC) encoder for Digital Radio Mondiale,
   integrated from libxaac. The API mirrors the conventions of libAACenc
   (aacenc_lib.h): opaque handle, create-time configuration, per-frame encode.

   Bitstream output is written into a caller-provided HANDLE_FDK_BITSTREAM.
   DRM audio-super-frame framing is provided by the TT_DRM transport writer in
   libMpegTPEnc (transportEnc_* API); this module emits raw USAC access units.

   Scope restriction: mono and stereo only (MODE_1 / MODE_2). Multichannel and
   MPS configurations are rejected at drmXheEnc_Open() with
   AACENC_INVALID_CONFIG, which keeps the compile-time worst-case memory
   bounds (see drm_xhe_enc_ram.h) small and exact.
   -------------------------------------------------------------------------- */

#ifndef DRM_XHE_ENC_H
#define DRM_XHE_ENC_H

#include "machine_type.h"
#include "FDK_audio.h"
#include "FDK_bitstream.h"

/* Error domain: all libxaac-internal error codes are translated to fdk-aac's
   native AACENC_ERROR enumeration (see DrmXhe_TranslateError() internally). */
#include "aacenc_lib.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque encoder handle, patterned after HANDLE_AACENCODER. */
typedef struct DRM_XHE_ENCODER *HANDLE_DRM_XHE_ENC;

/**
 * eSBR operating ratio. Values map onto libxaac's core-coder frame-length
 * index (ccfl_idx): OFF -> 1 (ccfl 1024, no eSBR), 8:3 -> 2 (ccfl 768),
 * 2:1 -> 3 (ccfl 1024), 4:1 -> 4 (ccfl 1024).
 */
typedef enum {
  DRM_XHE_SBR_OFF = 0,
  DRM_XHE_SBR_RATIO_8_3 = 1,
  DRM_XHE_SBR_RATIO_2_1 = 2,
  DRM_XHE_SBR_RATIO_4_1 = 3
} DRM_XHE_SBR_RATIO;

/**
 * Create-time configuration. libxaac fixes its memory layout at create time,
 * so unlike aacEncoder_SetParam() all parameters are supplied to
 * drmXheEnc_Open() and are immutable for the life of the handle.
 */
typedef struct {
  UINT sampleRate;          /*!< Input PCM sample rate in Hz. */
  UINT bitRate;             /*!< Encoder bit rate in bit/s. Also determines the
                                 fixed DRM audio-super-frame length:
                                 bitRate * superFrameMs / (8*1000) bytes. */
  CHANNEL_MODE channelMode; /*!< MODE_1 or MODE_2 only; anything else is
                                 rejected with AACENC_INVALID_CONFIG. */
  DRM_XHE_SBR_RATIO sbrRatio; /*!< eSBR ratio, see DRM_XHE_SBR_RATIO. */
  UCHAR useDrc;             /*!< 1: embed MPEG-D DRC metadata (loudness
                                 leveling per DRM requirements), 0: off. */
  INT drcTargetLoudness;    /*!< DRC target loudness in LUFS x 4 domain,
                                 only evaluated if useDrc == 1. */
  UINT superFrameMs;        /*!< DRM audio super frame duration in ms.
                                 0 selects the default (400 ms). */
} DRM_XHE_ENC_CONFIG;

/**
 * Static information about an opened encoder instance, patterned after
 * AACENC_InfoStruct.
 */
typedef struct {
  UINT frameLength;    /*!< PCM samples per channel consumed per access unit
                            (core-coder frame length x eSBR ratio). */
  UINT nDelaySamples;  /*!< Encoder input-side latency in PCM samples per
                            channel: input consumed before the first access
                            unit is emitted (the engine runs with delay
                            adjustment enabled, so decoded output is time-
                            aligned beyond this). Value is 0 until the first
                            AU has been produced; re-query via
                            drmXheEnc_GetInfo() after encoding starts. */
  UINT maxAuBytes;     /*!< Worst-case size of one encoded access unit. */
  UINT superFrameBytes; /*!< Fixed DRM audio super frame length in bytes,
                             derived from bitRate and superFrameMs. */
  UCHAR audioConfig[64]; /*!< xHE-AAC audio configuration for the DRM SDC
                              (Service Description Channel); consumable by
                              transportDec_DrmRawSdcAudioConfig_Check(). */
  UINT audioConfigBytes; /*!< Valid bytes in audioConfig. */
} DRM_XHE_ENC_INFO;

/**
 * Allocate and configure a DRM xHE-AAC encoder instance.
 *
 * All memory is claimed here, up front, from fdk-aac's memory manager
 * (genericStds.h) using compile-time worst-case sizes; no allocation happens
 * during encoding. See drm_xhe_enc_ram.h.
 *
 * \param phEnc  Pointer to an encoder handle; initialized on success.
 * \param config Create-time configuration; validated before any allocation.
 *
 * \return AACENC_OK on success,
 *         AACENC_INVALID_CONFIG on parameter/scope violations (incl. any
 *         channel configuration other than mono/stereo),
 *         AACENC_MEMORY_ERROR if a memory bound is exceeded.
 */
AACENC_ERROR drmXheEnc_Open(HANDLE_DRM_XHE_ENC *phEnc,
                            const DRM_XHE_ENC_CONFIG *config);

/**
 * Retrieve static instance information (frame length, delay, SDC config).
 */
AACENC_ERROR drmXheEnc_GetInfo(const HANDLE_DRM_XHE_ENC hEnc,
                               DRM_XHE_ENC_INFO *pInfo);

/**
 * Encode one access unit.
 *
 * Consumes exactly DRM_XHE_ENC_INFO::frameLength PCM samples per channel
 * (interleaved) and writes one raw USAC access unit into hBsAu via
 * FDKwriteBits(). The caller passes the resulting AU to the TT_DRM transport
 * writer (transportEnc_WriteAccessUnit()) for super-frame packing, or stores
 * it raw.
 *
 * \param hEnc         Encoder handle.
 * \param inputBuffer  Interleaved input PCM.
 * \param nInputSamples Number of valid samples in inputBuffer (all channels).
 * \param hBsAu        Output bitstream (BS_WRITER) the AU is serialized into.
 *
 * \return AACENC_OK, or AACENC_ENCODE_ERROR on a fatal libxaac error
 *         (non-fatal libxaac codes are absorbed, encoding continues).
 */
AACENC_ERROR drmXheEnc_EncodeFrame(HANDLE_DRM_XHE_ENC hEnc,
                                   const INT_PCM *inputBuffer,
                                   const UINT nInputSamples,
                                   HANDLE_FDK_BITSTREAM hBsAu);

/**
 * Close the encoder and release all memory back to the FDK memory manager.
 * Sets *phEnc to NULL. Safe to call with *phEnc == NULL.
 */
AACENC_ERROR drmXheEnc_Close(HANDLE_DRM_XHE_ENC *phEnc);

/**
 * Total worst-case heap requirement of one encoder instance in bytes
 * (sum of the GetRequiredMem*() of all internal memory blocks).
 * Available without opening an instance, for static memory budgeting.
 */
UINT drmXheEnc_GetRequiredMem(void);

#ifdef __cplusplus
}
#endif

#endif /* DRM_XHE_ENC_H */
