/* -----------------------------------------------------------------------------
   DRM xHE-AAC encoder library (libDRMxheEnc) — public API (FDK side)

   FDK-side TU: fdk-aac headers only, never vendored ones (see
   drm_xhe_enc_pool.h). All vendored-engine work is delegated across the
   neutral bridge (drm_xhe_vendor_bridge.h) to drm_xhe_vendor_bridge.cpp.

   Lifecycle:
     drmXheEnc_Open   — validate config (mono/stereo gate), claim ALL memory
                        from the FDK memory manager (Option B worst case),
                        arm the pool dispenser, bridge to ixheaace_create().
     drmXheEnc_EncodeFrame — bridge to ixheaace_process(); serialize the
                        returned access unit into the caller's
                        HANDLE_FDK_BITSTREAM via FDKwriteBits().
     drmXheEnc_Close  — bridge to ixheaace_delete() (frees are no-ops through
                        the pool hook), then return every block via
                        FreeRam_drmXhe_*().
   -------------------------------------------------------------------------- */

#include "drm_xhe_enc.h"
#include "drm_xhe_enc_ram.h"
#include "drm_xhe_vendor_bridge.h"

/* --------------------------------------------------------------------------
   Error translation (rule 4): raw vendored IA_ERRORCODE -> AACENC_ERROR.
   -------------------------------------------------------------------------- */
AACENC_ERROR DrmXhe_TranslateError(INT err) {
  switch (DrmXheBridge_ClassifyError((int)err)) {
    case DRMXHE_ERRCLASS_OK:
      return AACENC_OK;
    case DRMXHE_ERRCLASS_MEM:
      return AACENC_MEMORY_ERROR;
    case DRMXHE_ERRCLASS_CONFIG:
      return AACENC_INVALID_CONFIG;
    case DRMXHE_ERRCLASS_FATAL:
    default:
      return AACENC_ENCODE_ERROR;
  }
}

/* --------------------------------------------------------------------------
   Internal helpers
   -------------------------------------------------------------------------- */

static void DrmXhe_FreeAllMem(HANDLE_DRM_XHE_ENC hEnc) {
  DRMXHE_MEM_POOL *pool = &hEnc->memPool;

  FreeRam_drmXhe_ApiMem(&pool->pBlock[DRMXHE_MEM_API]);
  FreeRam_drmXhe_MemTabs(&pool->pBlock[DRMXHE_MEM_MEMTABS]);
  FreeRam_drmXhe_Persist(&pool->pBlock[DRMXHE_MEM_PERSIST]);
  FreeRam_drmXhe_Scratch(&pool->pBlock[DRMXHE_MEM_SCRATCH]);
  FreeRam_drmXhe_InBuf(&pool->pBlock[DRMXHE_MEM_INPUT]);
  FreeRam_drmXhe_OutBuf(&pool->pBlock[DRMXHE_MEM_OUTPUT]);
}

static int DrmXhe_MapSbrRatioToCcflIdx(DRM_XHE_SBR_RATIO ratio) {
  switch (ratio) {
    case DRM_XHE_SBR_RATIO_8_3:
      return 2;
    case DRM_XHE_SBR_RATIO_2_1:
      return 3;
    case DRM_XHE_SBR_RATIO_4_1:
      return 4;
    case DRM_XHE_SBR_OFF:
    default:
      return 1; /* ccfl 1024, no eSBR */
  }
}

/* --------------------------------------------------------------------------
   Public API
   -------------------------------------------------------------------------- */

AACENC_ERROR drmXheEnc_Open(HANDLE_DRM_XHE_ENC *phEnc,
                            const DRM_XHE_ENC_CONFIG *config) {
  HANDLE_DRM_XHE_ENC hEnc;
  DRMXHE_MEM_POOL *pool;
  DRMXHE_BRIDGE_CONFIG bridgeCfg;
  DRMXHE_BRIDGE_INFO bridgeInfo;
  UINT nChannels;

  if (phEnc == NULL || config == NULL) {
    return AACENC_INVALID_HANDLE;
  }
  *phEnc = NULL;

  /* DRM scope gate: mono/stereo only; anything else (incl. any MPS or
     multichannel mode) is rejected before a single byte is allocated. */
  switch (config->channelMode) {
    case MODE_1:
      nChannels = 1;
      break;
    case MODE_2:
      nChannels = 2;
      break;
    default:
      return AACENC_INVALID_CONFIG;
  }
  if (config->sampleRate == 0 || config->bitRate == 0) {
    return AACENC_INVALID_CONFIG;
  }

  hEnc = GetRam_drmXhe_Handle(0);
  if (hEnc == NULL) {
    return AACENC_MEMORY_ERROR;
  }

  /* Claim the five engine regions up front (Option B worst case) and arm
     the dispenser. Block sizes must match the C_AALLOC_MEM definitions in
     drm_xhe_enc_ram.cpp. */
  pool = &hEnc->memPool;
  pool->pBlock[DRMXHE_MEM_API] = GetRam_drmXhe_ApiMem(0);
  pool->blockSize[DRMXHE_MEM_API] = DrmXhe_GetApiMemSizeMax() + DRMXHE_ALIGN_SLACK;
  pool->pBlock[DRMXHE_MEM_MEMTABS] = GetRam_drmXhe_MemTabs(0);
  pool->blockSize[DRMXHE_MEM_MEMTABS] = DrmXhe_GetMemTabsSizeMax() + DRMXHE_ALIGN_SLACK;
  pool->pBlock[DRMXHE_MEM_PERSIST] = GetRam_drmXhe_Persist(0);
  pool->blockSize[DRMXHE_MEM_PERSIST] = DrmXhe_GetPersistSizeMax() + DRMXHE_ALIGN_SLACK;
  pool->pBlock[DRMXHE_MEM_SCRATCH] = GetRam_drmXhe_Scratch(0);
  pool->blockSize[DRMXHE_MEM_SCRATCH] = DrmXhe_GetScratchSizeMax() + DRMXHE_ALIGN_SLACK;
  pool->pBlock[DRMXHE_MEM_INPUT] = GetRam_drmXhe_InBuf(0);
  pool->blockSize[DRMXHE_MEM_INPUT] = DRMXHE_INBUF_SIZE + DRMXHE_ALIGN_SLACK;
  pool->pBlock[DRMXHE_MEM_OUTPUT] = GetRam_drmXhe_OutBuf(0);
  pool->blockSize[DRMXHE_MEM_OUTPUT] = DRMXHE_OUTBUF_SIZE + DRMXHE_ALIGN_SLACK;
  pool->nextRegion = DRMXHE_MEM_API;

  for (int i = 0; i < DRMXHE_MEM_N; i++) {
    if (pool->pBlock[i] == NULL) {
      DrmXhe_FreeAllMem(hEnc);
      FreeRam_drmXhe_Handle(&hEnc);
      return AACENC_MEMORY_ERROR;
    }
  }

  hEnc->encConfig = *config;

  bridgeCfg.sampleRate = config->sampleRate;
  bridgeCfg.bitRate = config->bitRate;
  bridgeCfg.nChannels = nChannels;
  bridgeCfg.ccflIdx = DrmXhe_MapSbrRatioToCcflIdx(config->sbrRatio);
  bridgeCfg.useDrc = (config->useDrc != 0) ? 1 : 0;

  FDKmemclear(&bridgeInfo, sizeof(bridgeInfo));

  hEnc->lastXheError = (INT)DrmXheBridge_Create(
      hEnc->xheInCfg.mem, DRMXHE_INCFG_MEM_SIZE, hEnc->xheOutCfg.mem,
      DRMXHE_OUTCFG_MEM_SIZE, hEnc->drcCfg.mem, DRMXHE_DRC_CFG_MEM_SIZE, pool,
      &bridgeCfg, &bridgeInfo, hEnc->encInfo.audioConfig,
      (UINT)sizeof(hEnc->encInfo.audioConfig));

  if (DrmXheBridge_ClassifyError((int)hEnc->lastXheError) != DRMXHE_ERRCLASS_OK) {
    AACENC_ERROR result = DrmXhe_TranslateError(hEnc->lastXheError);
    /* The vendored create already ran its cleanup through the no-op free
       hook; only the FDK blocks and the handle remain to release. */
    DrmXhe_FreeAllMem(hEnc);
    FreeRam_drmXhe_Handle(&hEnc);
    return result;
  }

  /* Post-conditions of a healthy create: all six regions dispensed. */
  if (pool->nextRegion != (UINT)DRMXHE_MEM_N) {
    DrmXheBridge_Delete(hEnc->xheOutCfg.mem);
    DrmXhe_FreeAllMem(hEnc);
    FreeRam_drmXhe_Handle(&hEnc);
    return AACENC_INIT_ERROR;
  }

  hEnc->encInfo.frameLength =
      bridgeInfo.inputSizeBytes / (nChannels * (bridgeInfo.pcmWordSizeBits / 8));
  hEnc->encInfo.nDelaySamples = 0; /* exposed with the transport work (M4) */
  hEnc->encInfo.maxAuBytes = DRMXHE_OUTBUF_SIZE;
  {
    UINT sfMs = (config->superFrameMs != 0) ? config->superFrameMs : 400;
    hEnc->encInfo.superFrameBytes = (config->bitRate * sfMs) / 8000;
  }
  hEnc->encInfo.audioConfigBytes =
      (bridgeInfo.configBytes <= (UINT)sizeof(hEnc->encInfo.audioConfig))
          ? bridgeInfo.configBytes
          : (UINT)sizeof(hEnc->encInfo.audioConfig);

  *phEnc = hEnc;
  return AACENC_OK;
}

AACENC_ERROR drmXheEnc_GetInfo(const HANDLE_DRM_XHE_ENC hEnc,
                               DRM_XHE_ENC_INFO *pInfo) {
  if (hEnc == NULL) {
    return AACENC_INVALID_HANDLE;
  }
  if (pInfo == NULL) {
    return AACENC_UNSUPPORTED_PARAMETER;
  }
  *pInfo = hEnc->encInfo;
  return AACENC_OK;
}

AACENC_ERROR drmXheEnc_EncodeFrame(HANDLE_DRM_XHE_ENC hEnc,
                                   const INT_PCM *inputBuffer,
                                   const UINT nInputSamples,
                                   HANDLE_FDK_BITSTREAM hBsAu) {
  const UCHAR *pAu = NULL;
  UINT auBytes = 0;

  if (hEnc == NULL) {
    return AACENC_INVALID_HANDLE;
  }
  if (inputBuffer == NULL || hBsAu == NULL) {
    return AACENC_UNSUPPORTED_PARAMETER;
  }

  hEnc->lastXheError = (INT)DrmXheBridge_EncodeFrame(
      hEnc->xheInCfg.mem, hEnc->xheOutCfg.mem, (const unsigned char *)inputBuffer,
      nInputSamples * (UINT)sizeof(INT_PCM), (const unsigned char **)&pAu,
      &auBytes);

  if (DrmXheBridge_ClassifyError((int)hEnc->lastXheError) != DRMXHE_ERRCLASS_OK) {
    return DrmXhe_TranslateError(hEnc->lastXheError);
  }

  /* Latency measurement: leading frames that produce no AU are the engine
     priming its delay lines. Finalized on the first AU. */
  if (!hEnc->firstAuSeen) {
    if (auBytes == 0) {
      hEnc->delayFrames++;
    } else {
      hEnc->firstAuSeen = 1;
      hEnc->encInfo.nDelaySamples = hEnc->delayFrames * hEnc->encInfo.frameLength;
    }
  }

  if (auBytes > 0) {
    if (FDKgetFreeBits(hBsAu) < (INT)(auBytes * 8)) {
      return AACENC_ENCODE_ERROR;
    }
    for (UINT i = 0; i < auBytes; i++) {
      FDKwriteBits(hBsAu, pAu[i], 8);
    }
    FDKsyncCache(hBsAu);
  }

  return AACENC_OK;
}

AACENC_ERROR drmXheEnc_Close(HANDLE_DRM_XHE_ENC *phEnc) {
  if (phEnc == NULL) {
    return AACENC_INVALID_HANDLE;
  }
  if (*phEnc == NULL) {
    return AACENC_OK;
  }

  /* Vendored delete: frees flow through the no-op pool hook. */
  DrmXheBridge_Delete((*phEnc)->xheOutCfg.mem);

  DrmXhe_FreeAllMem(*phEnc);
  FreeRam_drmXhe_Handle(phEnc); /* also sets *phEnc = NULL */

  return AACENC_OK;
}
