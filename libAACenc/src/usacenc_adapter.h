/*-----------------------------------------------------------------------------
Software License for The Fraunhofer FDK AAC Codec Library for Android

See NOTICE for the FDK AAC Codec license.

This file is part of the xHE-AAC (MPEG-D USAC) encoder extension added to
FDK-AAC. It bridges the public libAACenc API (aacenc_lib.h) to the vendored
Ittiam Systems USAC encoder in third_party/libxaac/encoder (Apache-2.0
licensed, see third_party/libxaac/NOTICE and third_party/libxaac/LICENSE).
------------------------------------------------------------------------- */

#ifndef USACENC_ADAPTER_H
#define USACENC_ADAPTER_H

#include "machine_type.h"
#include "FDK_audio.h"
#include "aacenc_lib.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct USAC_ENC_ADAPTER *HANDLE_USAC_ENC_ADAPTER;

/** USAC core coder mode, mirrors USAC_SWITCHED / USAC_ONLY_FD / USAC_ONLY_TD
 *  from the vendored Ittiam encoder (iusace_cnst.h). FD-only avoids the
 *  ACELP/TCX (LPD) speech coder path and is the safe default; SWITCHED
 *  enables the full USAC core (FD + LPD) which is generally preferable for
 *  speech-heavy DRM content. */
typedef enum {
  /* FD-only (value 0) is the default so that a zero-initialized encoder
   * handle (AACENCODER is FDKmemclear'd on aacEncOpen) picks the safe,
   * well-tested default mode without requiring an explicit SetParam call --
   * this matches the vendored Ittiam encoder's own CLI default. */
  USACENC_CODEC_MODE_FD_ONLY = 0,
  USACENC_CODEC_MODE_SWITCHED = 1,
  USACENC_CODEC_MODE_TD_ONLY = 2
} USACENC_CODEC_MODE;

/** Preset operating points typical for Digital Radio Mondiale (DRM) xHE-AAC
 *  audio services per ETSI ES 201 980. Core sample rates of 12/24 kHz apply
 *  to robustness modes A-D, 24/48 kHz to mode E. These are reasonable
 *  starting points, not a substitute for verifying exact bit rate /
 *  robustness-mode requirements against the standard for a given service.
 *
 *  NOTE: the vendored Ittiam USAC encoder currently only supports two fixed
 *  bit rate operating points, 64000 and 96000 bps (see README_enc.md under
 *  third_party/libxaac); any other requested bit rate is silently clamped
 *  to the nearer of the two. These presets therefore target 64000/96000 bps
 *  rather than the much lower bit rates typical DRM services actually use
 *  -- true low-bitrate DRM xHE-AAC is not yet achievable through this
 *  integration until the vendored encoder supports more operating points. */
typedef enum {
  USACENC_DRM_PROFILE_NONE = 0,   /*!< Generic USAC, no DRM preset applied. */
  USACENC_DRM_PROFILE_LOW = 1,    /*!< ~12 kHz core, mono, 64000 bps (lowest
                                        bit rate the vendored encoder
                                        currently supports). */
  USACENC_DRM_PROFILE_MEDIUM = 2, /*!< ~24 kHz core, 64000 bps. */
  USACENC_DRM_PROFILE_HIGH = 3    /*!< ~24 kHz core, 96000 bps. */
} USACENC_DRM_PROFILE;

typedef struct {
  UINT sampleRate;    /*!< Input PCM sample rate in Hz. */
  UINT bitRate;        /*!< Target bit rate in bits/second. */
  UINT nChannels;       /*!< Number of input channels (1 or 2). */
  UINT useAdts;          /*!< 1: wrap access units with ADTS-like framing.
                               0: self-contained raw USAC elementary stream
                               (config embedded once at stream start). */
  USACENC_CODEC_MODE codecMode;
  USACENC_DRM_PROFILE drmProfile; /*!< Applies a DRM-typical preset; overrides
                                        sampleRate/bitRate derived fields as
                                        documented in usacenc_adapter.cpp. */
  UINT frameLength; /*!< 0 = automatic (1024), else 768 or 1024. */
} USAC_ENC_PARAMS;

/**
 * Opens (creates) a USAC/xHE-AAC encoder instance wrapping the vendored
 * Ittiam ixheaace encoder.
 */
AACENC_ERROR usacEncOpen(HANDLE_USAC_ENC_ADAPTER *phUsacEnc,
                         const USAC_ENC_PARAMS *params);

/** Returns the core coder frame length (in samples/channel) and the total
 *  algorithmic delay (in samples/channel) chosen for this instance. */
AACENC_ERROR usacEncGetFrameLength(HANDLE_USAC_ENC_ADAPTER hUsacEnc,
                                   INT *frameLength, INT *nDelay);

/**
 * Encodes exactly one core coder frame worth of interleaved INT_PCM audio
 * samples (nInputSamples total across all channels) and writes the produced
 * access unit(s) to pOutBuf. On the very first call, the output additionally
 * contains the USAC AudioSpecificConfig once (self-contained stream start).
 */
AACENC_ERROR usacEncEncodeFrame(HANDLE_USAC_ENC_ADAPTER hUsacEnc,
                                const INT_PCM *pInput, INT nInputSamples,
                                UCHAR *pOutBuf, INT outBufSize,
                                INT *pOutBytes);

/** Releases all resources associated with hUsacEnc. */
void usacEncClose(HANDLE_USAC_ENC_ADAPTER *phUsacEnc);

#ifdef __cplusplus
}
#endif

#endif /* USACENC_ADAPTER_H */
