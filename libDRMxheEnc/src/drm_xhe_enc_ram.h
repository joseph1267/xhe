/* -----------------------------------------------------------------------------
   DRM xHE-AAC encoder library (libDRMxheEnc) — internal RAM layout (FDK side)

   Vendoring architecture: the libxaac encoder sources are physically vendored
   into src/libxaac_vendor/ (encoder/, encoder/drc_src/, common/) and compiled
   into this module. The external top-level libxaac/ checkout is reference
   only. Local modifications to vendored files are minimal and every modified
   line is tagged "DRM_XHE_VENDOR_PATCH" for future vendor refreshes.

   This header is FDK-side only: it must never include vendored libxaac
   headers (macro collisions — see drm_xhe_enc_pool.h). Vendored config
   structs are therefore embedded in the handle as opaque, size-bounded,
   alignment-safe storage; only vendor-side TUs cast them to their real types.

   Memory model (Option B — compile-time worst-case sizing): see
   drm_xhe_enc_pool.h for the six-region pool and the pinned worst-case
   bounds; drm_xhe_enc_ram.cpp defines the FDK memory blocks below via the
   C_ALLOC_MEM / C_AALLOC_MEM macro family (genericStds.h). All memory is
   claimed at drmXheEnc_Open(); the pool dispenser hands it to the vendored
   encoder and fails hard rather than ever exceeding a pinned bound.
   -------------------------------------------------------------------------- */

#ifndef DRM_XHE_ENC_RAM_H
#define DRM_XHE_ENC_RAM_H

#include "genericStds.h"
#include "drm_xhe_enc.h"
#include "drm_xhe_enc_pool.h"

/* Opaque, alignment-safe backing storage for a vendored struct of at most
   `size` bytes (bound static_asserted vendor-side). */
#define DRMXHE_OPAQUE_STORAGE(name, size) \
  union {                                 \
    UCHAR mem[size];                      \
    void *alignPtr;                       \
    double alignFp;                       \
  } name

/**
 * Encoder instance, patterned after struct AACENCODER (aacenc_lib.cpp).
 * Allocated via GetRam_drmXhe_Handle(); everything the vendored encoder
 * touches lives in memPool blocks; nothing here is separately heap-allocated.
 */
struct DRM_XHE_ENCODER {
  DRM_XHE_ENC_CONFIG encConfig; /* immutable copy of the open() config */
  DRM_XHE_ENC_INFO encInfo;     /* derived at open() */

  DRMXHE_MEM_POOL memPool;

  /* Vendored libxaac interop state, opaque at FDK side. Vendor-side glue
     casts: xheInCfg -> ixheaace_input_config (with pv_drc_cfg pointing at
     drcCfg below), xheOutCfg -> ixheaace_output_config (carries the pool
     dispenser hooks and pv_mem_ctx = &memPool, DRM_XHE_VENDOR_PATCH). */
  DRMXHE_OPAQUE_STORAGE(xheInCfg, DRMXHE_INCFG_MEM_SIZE);
  DRMXHE_OPAQUE_STORAGE(xheOutCfg, DRMXHE_OUTCFG_MEM_SIZE);
  DRMXHE_OPAQUE_STORAGE(drcCfg, DRMXHE_DRC_CFG_MEM_SIZE);

  INT lastXheError; /* last raw libxaac IA_ERRORCODE, for diagnostics; the
                       public API only ever returns AACENC_ERROR */

  /* Encoder-latency measurement: the vendored engine (delay adjustment on)
     emits no AU while priming its delay lines; the count of empty leading
     frames times frameLength is the input-side latency surfaced as
     DRM_XHE_ENC_INFO::nDelaySamples once the first AU appears. */
  UINT delayFrames;
  UCHAR firstAuSeen;
};

/* ------------------------------------------------------------------------- */
/* FDK memory declarations (definitions with C_ALLOC_MEM / C_AALLOC_MEM in    */
/* drm_xhe_enc_ram.cpp)                                                       */
/* ------------------------------------------------------------------------- */

H_ALLOC_MEM(Ram_drmXhe_Handle, DRM_XHE_ENCODER)
H_ALLOC_MEM(Ram_drmXhe_ApiMem, UCHAR)
H_ALLOC_MEM(Ram_drmXhe_MemTabs, UCHAR)
H_ALLOC_MEM(Ram_drmXhe_Persist, UCHAR)
H_ALLOC_MEM(Ram_drmXhe_Scratch, UCHAR)
H_ALLOC_MEM(Ram_drmXhe_InBuf, UCHAR)
H_ALLOC_MEM(Ram_drmXhe_OutBuf, UCHAR)

/* IA_ERRORCODE -> AACENC_ERROR translation (fatal bit: 0x80000000). */
AACENC_ERROR DrmXhe_TranslateError(INT err);

#endif /* DRM_XHE_ENC_RAM_H */
