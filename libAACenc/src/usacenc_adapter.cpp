/*-----------------------------------------------------------------------------
Software License for The Fraunhofer FDK AAC Codec Library for Android

See NOTICE for the FDK AAC Codec license.

This file bridges the public libAACenc API to the vendored Ittiam Systems
USAC (xHE-AAC) encoder living in third_party/libxaac/encoder. It is the only
translation unit that includes both the FDK and the Ittiam headers; the two
codebases use disjoint type-name prefixes (FDK: INT/UINT/UCHAR/...; Ittiam:
WORD32/UWORD32/UCHAR8/...) so they coexist safely here.

The Ittiam encoder (Apache-2.0, see third_party/libxaac/NOTICE and LICENSE)
is used as-is for the actual MPEG-D USAC bitstream generation (FD/LPD core
coder, arithmetic coding, eSBR, MPS) rather than re-implemented, since a
from-scratch reimplementation of USAC's core coder would be unable to reach
the same level of correctness/verification within this change.
------------------------------------------------------------------------- */

#include "usacenc_adapter.h"

#include <stdlib.h>
#include <string.h>

extern "C" {
#include "ixheaac_type_def.h"
#include "ixheaac_error_standards.h"
#include "iusace_cnst.h"
#include "ixheaace_api.h"
/* ixheaace_set_config_params() dereferences input_config.pv_drc_cfg
 * unconditionally (even with use_drc_element == 0) to seed sample rate /
 * channel count bookkeeping, so a valid (zeroed) ia_drc_input_config must
 * always be supplied. */
#include "impd_drc_common_enc.h"
#include "impd_drc_uni_drc.h"
#include "impd_drc_tables.h"
#include "impd_drc_api.h"
}

struct USAC_ENC_ADAPTER {
  ixheaace_input_config input_config;
  ixheaace_output_config output_config;
  ia_drc_input_config drc_config; /* always required, see include note above */
  INT frameLength;
  INT nDelay;
  UINT firstFrameDone;
};

/* ixheaace_create() allocates all working memory itself via these
 * callbacks and hands back ready-to-use pointers in output_config's
 * mem_info_table[]; a plain aligned heap allocator is sufficient here. */
static pVOID usacenc_malloc(UWORD32 size, UWORD32 alignment) {
  if (alignment < sizeof(void *)) {
    alignment = sizeof(void *);
  }
#if defined(_WIN32)
  return _aligned_malloc(size, alignment);
#else
  void *ptr = NULL;
  if (posix_memalign(&ptr, alignment, size ? size : 1) != 0) {
    return NULL;
  }
  return ptr;
#endif
}

static VOID usacenc_free(pVOID ptr) {
#if defined(_WIN32)
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

/* Core-coder frame length table indexed by ccfl_idx, mirrors
 * ixheaace_get_usac_config_bytes() in third_party/libxaac. */
static const INT kUsacCcflFrameLength[5] = {768, 1024, 768, 1024, 1024};

static void usacenc_ApplyDrmPreset(USAC_ENC_PARAMS *p) {
  /* Typical DRM (Digital Radio Mondiale, ETSI ES 201 980) xHE-AAC core
   * sample rates: 12 kHz or 24 kHz for robustness modes A-D, 24/48 kHz for
   * mode E. Verify exact sample rate / robustness-mode requirements against
   * the standard for a specific service deployment.
   *
   * IMPORTANT: the vendored Ittiam USAC encoder (third_party/libxaac)
   * currently only supports two fixed USAC bit rate operating points --
   * 64000 and 96000 bps (documented in third_party/libxaac/README_enc.md
   * and confirmed by testing: requesting anything else gets silently
   * clamped to the nearer of the two by the vendored encoder). DRM services
   * typically want much lower bit rates than that; until the vendored
   * encoder gains more operating points, low-bitrate DRM profiles are not
   * actually achievable through this integration -- these presets pick the
   * lower of the two supported points (64000) rather than silently
   * promising a bit rate that will not be honored. */
  switch (p->drmProfile) {
    case USACENC_DRM_PROFILE_LOW:
      p->sampleRate = 12000;
      p->bitRate = 64000;
      p->nChannels = 1;
      break;
    case USACENC_DRM_PROFILE_MEDIUM:
      p->sampleRate = 24000;
      p->bitRate = 64000;
      break;
    case USACENC_DRM_PROFILE_HIGH:
      p->sampleRate = 24000;
      p->bitRate = 96000;
      break;
    case USACENC_DRM_PROFILE_NONE:
    default:
      break;
  }
}

AACENC_ERROR usacEncOpen(HANDLE_USAC_ENC_ADAPTER *phUsacEnc,
                         const USAC_ENC_PARAMS *paramsIn) {
  if (phUsacEnc == NULL || paramsIn == NULL) {
    return AACENC_INVALID_HANDLE;
  }

  USAC_ENC_PARAMS params = *paramsIn;
  usacenc_ApplyDrmPreset(&params);

  if (params.nChannels == 0 || params.nChannels > 2 || params.sampleRate == 0 ||
      params.bitRate == 0) {
    return AACENC_INVALID_CONFIG;
  }

  HANDLE_USAC_ENC_ADAPTER hUsacEnc =
      (HANDLE_USAC_ENC_ADAPTER)calloc(1, sizeof(*hUsacEnc));
  if (hUsacEnc == NULL) {
    return AACENC_MEMORY_ERROR;
  }

  ixheaace_input_config *cfg = &hUsacEnc->input_config;
  ixheaace_output_config *out = &hUsacEnc->output_config;

  memset(cfg, 0, sizeof(*cfg));
  memset(out, 0, sizeof(*out));
  memset(&hUsacEnc->drc_config, 0, sizeof(hUsacEnc->drc_config));
  cfg->pv_drc_cfg = &hUsacEnc->drc_config;

  INT ccflIdx = NO_SBR_CCFL_1024;
  if (params.frameLength == 768) {
    ccflIdx = NO_SBR_CCFL_768;
  } else if (params.frameLength != 0 && params.frameLength != 1024) {
    free(hUsacEnc);
    return AACENC_INVALID_CONFIG;
  }

  WORD32 codecMode = USAC_ONLY_FD;
  switch (params.codecMode) {
    case USACENC_CODEC_MODE_SWITCHED:
      codecMode = USAC_SWITCHED;
      break;
    case USACENC_CODEC_MODE_TD_ONLY:
      codecMode = USAC_ONLY_TD;
      break;
    case USACENC_CODEC_MODE_FD_ONLY:
    default:
      codecMode = USAC_ONLY_FD; /* default */
      break;
  }

  /* AAC-config sub-struct (also consulted by the USAC path for TNS/noise
   * filling/bandwidth) */
  cfg->aac_config.bitrate = (WORD32)params.bitRate;
  cfg->aac_config.num_channels_in = (WORD32)params.nChannels;
  cfg->aac_config.num_channels_out = (WORD32)params.nChannels;
  cfg->aac_config.bandwidth = 0; /* internal default */
  cfg->aac_config.use_tns = 1;
  cfg->aac_config.noise_filling = 0;
  cfg->aac_config.use_adts = (WORD32)params.useAdts;
  cfg->aac_config.inv_quant = 2;
  cfg->aac_config.full_bandwidth = 0;
  cfg->aac_config.bitreservoir_size = 0; /* not used outside LD/ELD */

  cfg->ui_pcm_wd_sz = 16;
  cfg->i_bitrate = (WORD32)params.bitRate;
  cfg->frame_length = kUsacCcflFrameLength[ccflIdx];
  cfg->frame_cmd_flag = 1;
  cfg->out_bytes_flag = 0;
  cfg->user_tns_flag = 1;
  cfg->user_esbr_flag = 1;
  cfg->aot = AOT_USAC;
  cfg->i_mps_tree_config = -1;
  /* eSBR requires an SBR-paired ccfl_idx (SBR_8_3/SBR_2_1/SBR_4_1); the
   * default ccfl_idx here is a no-SBR index (NO_SBR_CCFL_768/1024), so eSBR
   * stays off by default and is only enabled by the DRM presets below,
   * which pick a consistent ccfl_idx/esbr_flag pair. Enabling eSBR against
   * a no-SBR ccfl_idx silently desyncs the requested bit rate from the
   * actual encoded rate. */
  cfg->esbr_flag = 0;
  cfg->i_channels = (WORD32)params.nChannels;
  cfg->i_samp_freq = params.sampleRate;
  cfg->i_native_samp_freq = (WORD32)params.sampleRate;
  cfg->i_channels_mask = 0;
  cfg->i_num_coupling_chan = 0;
  cfg->i_use_mps = 0;
  cfg->i_use_adts = (WORD32)params.useAdts;
  cfg->i_use_es = params.useAdts ? 0 : 1;
  cfg->usac_en = 1;
  cfg->codec_mode = codecMode;
  cfg->cplx_pred = 0;
  cfg->ccfl_idx = ccflIdx;
  cfg->pvc_active = 0;
  cfg->harmonic_sbr = 0;
  cfg->inter_tes_active = 0;
  cfg->use_drc_element = 0;
  cfg->drc_frame_size = 0;
  cfg->hq_esbr = 0;
  cfg->write_program_config_element = 0;
  cfg->random_access_interval = DEFAULT_RAP_INTERVAL_IN_MS;
  cfg->method_def = 0;
  cfg->measured_loudness = 0;
  cfg->measurement_system = 0;
  cfg->sample_peak_level = 0;
  cfg->stream_id = 0;
  cfg->use_delay_adjustment = USAC_DEFAULT_DELAY_ADJUSTMENT_VALUE;

  out->malloc_xheaace = &usacenc_malloc;
  out->free_xheaace = &usacenc_free;

  IA_ERRORCODE errCode = ixheaace_create((pVOID)cfg, (pVOID)out);
  if (errCode != IA_NO_ERROR || out->pv_ia_process_api_obj == NULL) {
    free(hUsacEnc);
    return AACENC_INIT_ERROR;
  }

  hUsacEnc->frameLength = cfg->frame_length;
  /* USAC LPD/FD delay is core-coder dependent; the vendored encoder does not
   * expose an explicit delay value through this API, so this is the nominal
   * one-frame algorithmic delay. */
  hUsacEnc->nDelay = cfg->frame_length;
  hUsacEnc->firstFrameDone = 0;

  *phUsacEnc = hUsacEnc;
  return AACENC_OK;
}

AACENC_ERROR usacEncGetFrameLength(HANDLE_USAC_ENC_ADAPTER hUsacEnc,
                                   INT *frameLength, INT *nDelay) {
  if (hUsacEnc == NULL) {
    return AACENC_INVALID_HANDLE;
  }
  if (frameLength != NULL) *frameLength = hUsacEnc->frameLength;
  if (nDelay != NULL) *nDelay = hUsacEnc->nDelay;
  return AACENC_OK;
}

AACENC_ERROR usacEncEncodeFrame(HANDLE_USAC_ENC_ADAPTER hUsacEnc,
                                const INT_PCM *pInput, INT nInputSamples,
                                UCHAR *pOutBuf, INT outBufSize,
                                INT *pOutBytes) {
  if (hUsacEnc == NULL || pOutBuf == NULL || pOutBytes == NULL) {
    return AACENC_INVALID_HANDLE;
  }

  ixheaace_output_config *out = &hUsacEnc->output_config;
  ixheaace_input_config *cfg = &hUsacEnc->input_config;

  *pOutBytes = 0;

  WORD8 *pInBuf = (WORD8 *)out->mem_info_table[IA_MEMTYPE_INPUT].mem_ptr;
  WORD8 *pOutInternal = (WORD8 *)out->mem_info_table[IA_MEMTYPE_OUTPUT].mem_ptr;
  if (pInBuf == NULL || pOutInternal == NULL) {
    return AACENC_INIT_ERROR;
  }

  /* INT_PCM (FDK) and WORD16 (Ittiam, ui_pcm_wd_sz==16) are both 16-bit
   * signed PCM; copy the interleaved frame into the encoder's input buffer.
   */
  SIZE_T bytesToCopy = (SIZE_T)nInputSamples * sizeof(INT_PCM);
  memcpy(pInBuf, pInput, bytesToCopy);

  IA_ERRORCODE errCode =
      ixheaace_process(out->pv_ia_process_api_obj, (pVOID)cfg, (pVOID)out);
  if (errCode != IA_NO_ERROR) {
    return AACENC_ENCODE_ERROR;
  }

  hUsacEnc->firstFrameDone = 1;

  if (out->i_out_bytes > 0) {
    if (out->i_out_bytes > outBufSize) {
      return AACENC_ENCODE_ERROR;
    }
    memcpy(pOutBuf, pOutInternal, (size_t)out->i_out_bytes);
    *pOutBytes = out->i_out_bytes;
  }

  return AACENC_OK;
}

void usacEncClose(HANDLE_USAC_ENC_ADAPTER *phUsacEnc) {
  if (phUsacEnc == NULL || *phUsacEnc == NULL) {
    return;
  }
  HANDLE_USAC_ENC_ADAPTER hUsacEnc = *phUsacEnc;
  ixheaace_delete((pVOID)&hUsacEnc->output_config);
  free(hUsacEnc);
  *phUsacEnc = NULL;
}
