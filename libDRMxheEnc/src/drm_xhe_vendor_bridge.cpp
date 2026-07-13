/* -----------------------------------------------------------------------------
   DRM xHE-AAC encoder library (libDRMxheEnc) — vendor bridge (vendor side)

   Vendor-side TU: includes ONLY the vendored libxaac header chain plus the
   neutral bridge/pool headers — never fdk-aac headers (macro collisions, see
   drm_xhe_enc_pool.h).

   Drives the vendored engine (ixheaace_create / ixheaace_process /
   ixheaace_delete) against the opaque config storage owned by the FDK-side
   DRM_XHE_ENCODER handle. The input-config defaults below mirror the
   vendored testbench's USAC path (ixheaace_testbench.c) — the authoritative
   example of a correct create-time setup.
   -------------------------------------------------------------------------- */

#include <stdlib.h>
#include <string.h>

extern "C" {
/* --- vendored libxaac header chain (order matters; mirrors ixheaace_api.c) */
#include "ixheaac_type_def.h"
#include "ixheaac_constants.h"
#include "ixheaace_aac_constants.h"
#include "ixheaac_basic_ops32.h"
#include "ixheaac_basic_ops16.h"
#include "ixheaac_basic_ops40.h"
#include "ixheaac_basic_ops.h"
#include "ixheaac_error_standards.h"
#include "ixheaace_config_params.h"
#include "ixheaace_definitions.h"
#include "ixheaace_error_codes.h"
#include "iusace_bitbuffer.h"
#include "impd_drc_common_enc.h"
#include "impd_drc_uni_drc.h"
#include "impd_drc_tables.h"
#include "impd_drc_api.h"
#include "ixheaace_sbr_header.h"
#include "ixheaace_sbr_def.h"
#include "ixheaace_resampler.h"
#include "ixheaace_psy_const.h"
#include "ixheaace_tns.h"
#include "ixheaace_tns_params.h"
#include "ixheaace_rom.h"
#include "ixheaace_common_rom.h"
#include "ixheaace_bitbuffer.h"
#include "ixheaace_sbr_rom.h"
#include "ixheaace_sbr_main.h"
#include "ixheaace_api.h"
#include "ixheaace_memory_standards.h"
#include "iusace_cnst.h"
} /* extern "C" */

#include "drm_xhe_vendor_bridge.h"

/* ixheaace_create/process/delete are declared in ixheaace_api.h. */

int DrmXheBridge_Create(void *pInCfgMem, unsigned int inCfgMemSize,
                        void *pOutCfgMem, unsigned int outCfgMemSize,
                        void *pDrcCfgMem, unsigned int drcCfgMemSize,
                        DRMXHE_MEM_POOL *pPool, const DRMXHE_BRIDGE_CONFIG *pCfg,
                        DRMXHE_BRIDGE_INFO *pInfo, unsigned char *pConfigOut,
                        unsigned int configOutMax) {
  ixheaace_input_config *pInCfg = (ixheaace_input_config *)pInCfgMem;
  ixheaace_output_config *pOutCfg = (ixheaace_output_config *)pOutCfgMem;
  IA_ERRORCODE err;

  /* Defensive: opaque storage must hold the real structs (also enforced by
     static_asserts in drm_xhe_vendor_sizes.cpp). */
  if (inCfgMemSize < sizeof(ixheaace_input_config) ||
      outCfgMemSize < sizeof(ixheaace_output_config) ||
      drcCfgMemSize < sizeof(ia_drc_input_config)) {
    return IA_EXHEAACE_API_FATAL_MEM_ALLOC;
  }

  memset(pInCfg, 0, sizeof(*pInCfg));
  memset(pOutCfg, 0, sizeof(*pOutCfg));
  memset(pDrcCfgMem, 0, sizeof(ia_drc_input_config));

  /* --- input config: mirrors the vendored testbench USAC defaults --- */
  /* aac_config subset (iaace_aac_set_default_config + USAC overrides) */
  pInCfg->aac_config.bitrate = 48000;
  pInCfg->aac_config.inv_quant = 2;
  pInCfg->aac_config.use_tns = 1; /* USAC default */
  pInCfg->aac_config.bitreservoir_size = 384;

  pInCfg->ui_pcm_wd_sz = 16;
  pInCfg->i_samp_freq = (WORD32)pCfg->sampleRate;
  pInCfg->i_native_samp_freq = (WORD32)pCfg->sampleRate;
  pInCfg->i_channels = (WORD32)pCfg->nChannels;
  pInCfg->i_bitrate = (WORD32)pCfg->bitRate;
  pInCfg->frame_length = 1024;

  pInCfg->usac_en = 1; /* ixheaace_allocate() forces aot = AOT_USAC */
  pInCfg->aot = AOT_USAC;
  pInCfg->codec_mode = USAC_ONLY_FD;
  pInCfg->ccfl_idx = pCfg->ccflIdx;
  /* eSBR is implied by ccfl_idx; mark it user-decided so the library does
     not re-default it (testbench: user_esbr_flag). */
  pInCfg->esbr_flag = (pCfg->ccflIdx > NO_SBR_CCFL_1024) ? 1 : 0;
  pInCfg->user_esbr_flag = 1;
  pInCfg->pvc_active = 0;
  pInCfg->harmonic_sbr = 0;
  pInCfg->inter_tes_active = 0;
  pInCfg->hq_esbr = 0;

  pInCfg->i_use_mps = 0; /* DRM scope: mono/stereo, MPS rejected FDK-side */
  pInCfg->i_mps_tree_config = -1;
  pInCfg->i_use_adts = 0;
  pInCfg->i_use_es = 1; /* raw elementary stream: config header at create,
                           bare access units per process call */
  pInCfg->cplx_pred = 0;
  pInCfg->i_channels_mask = 0;
  pInCfg->i_num_coupling_chan = 0;

  pInCfg->use_drc_element = pCfg->useDrc ? 1 : 0;
  pInCfg->pv_drc_cfg = pDrcCfgMem; /* zeroed: is_loudness_configured == 0
                                      lets the library self-fill defaults
                                      via its measured-loudness path */
  pInCfg->method_def = METHOD_DEFINITION_PROGRAM_LOUDNESS;
  pInCfg->measurement_system = MEASUREMENT_SYSTEM_BS_1770_3;

  pInCfg->random_access_interval = 2000; /* periodic AudioPreRoll (ms):
                                            DRM receivers tune in mid-stream
                                            and error recovery needs RAPs —
                                            without them one lost AU
                                            un-decodes the entire remaining
                                            stream (usacIndependencyFlag) */
  pInCfg->stream_id = 0;
  pInCfg->use_delay_adjustment = USAC_DEFAULT_DELAY_ADJUSTMENT_VALUE;

  /* --- output config: install the FDK pool dispenser --- */
  /* DRM_XHE_VENDOR_PATCH hooks (pv_mem_ctx cookie) */
  pOutCfg->malloc_xheaace = DrmXhe_PoolMalloc;
  pOutCfg->free_xheaace = DrmXhe_PoolFree;
  pOutCfg->pv_mem_ctx = (pVOID)pPool;

  err = ixheaace_create((pVOID)pInCfg, (pVOID)pOutCfg);
  if (err & IA_FATAL_ERROR) {
    return err;
  }

  if (pInfo != NULL) {
    pInfo->inputSizeBytes = (unsigned int)pOutCfg->input_size;
    pInfo->pcmWordSizeBits = 16;
    pInfo->coreSampleRate = (unsigned int)pOutCfg->samp_freq;
    pInfo->downSamplingRatio = pOutCfg->down_sampling_ratio;
    pInfo->configBytes = (unsigned int)pOutCfg->i_out_bytes;
  }
  /* Raw-ES config header emitted at create time (testbench writes it as the
     stream prefix); for DRM this is the SDC audio-config source. */
  if (pConfigOut != NULL && pOutCfg->i_out_bytes > 0) {
    unsigned int n = (unsigned int)pOutCfg->i_out_bytes;
    if (n > configOutMax) {
      n = configOutMax;
    }
    memcpy(pConfigOut, pOutCfg->mem_info_table[IA_MEMTYPE_OUTPUT].mem_ptr, n);
  }

  return err;
}

int DrmXheBridge_EncodeFrame(void *pInCfgMem, void *pOutCfgMem,
                             const unsigned char *pPcm, unsigned int pcmBytes,
                             const unsigned char **ppAuData,
                             unsigned int *pAuBytes) {
  ixheaace_input_config *pInCfg = (ixheaace_input_config *)pInCfgMem;
  ixheaace_output_config *pOutCfg = (ixheaace_output_config *)pOutCfgMem;
  unsigned char *pInBuf =
      (unsigned char *)pOutCfg->mem_info_table[IA_MEMTYPE_INPUT].mem_ptr;
  unsigned int inputSize = (unsigned int)pOutCfg->input_size;
  IA_ERRORCODE err;

  if (pcmBytes > inputSize) {
    pcmBytes = inputSize;
  }
  memcpy(pInBuf, pPcm, pcmBytes);
  if (pcmBytes < inputSize) {
    /* final-frame zero padding, mirroring the vendored testbench */
    memset(pInBuf + pcmBytes, 0, inputSize - pcmBytes);
  }

  err = ixheaace_process(pOutCfg->pv_ia_process_api_obj, (pVOID)pInCfg,
                         (pVOID)pOutCfg);

  *ppAuData = (const unsigned char *)pOutCfg->mem_info_table[IA_MEMTYPE_OUTPUT].mem_ptr;
  *pAuBytes = (err & IA_FATAL_ERROR) ? 0 : (unsigned int)pOutCfg->i_out_bytes;

  return err;
}

int DrmXheBridge_Delete(void *pOutCfgMem) {
  return ixheaace_delete((pVOID)pOutCfgMem);
}

DRMXHE_ERRCLASS DrmXheBridge_ClassifyError(int err) {
  if (!(err & IA_FATAL_ERROR)) {
    return DRMXHE_ERRCLASS_OK; /* IA_NO_ERROR or recoverable */
  }
  if ((IA_ERRORCODE)err == IA_EXHEAACE_API_FATAL_MEM_ALLOC) {
    return DRMXHE_ERRCLASS_MEM;
  }
  /* Config-stage fatal ranges: API 0xFFFF8000.. and CONFIG 0xFFFF8800.. */
  if ((IA_ERRORCODE)err == IA_EXHEAACE_API_FATAL_UNSUPPORTED_AOT ||
      (((UWORD32)err & 0xFFFFFF00u) == 0xFFFF8800u)) {
    return DRMXHE_ERRCLASS_CONFIG;
  }
  return DRMXHE_ERRCLASS_FATAL;
}
