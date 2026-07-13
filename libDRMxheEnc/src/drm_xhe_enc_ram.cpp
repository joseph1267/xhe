/* -----------------------------------------------------------------------------
   DRM xHE-AAC encoder library (libDRMxheEnc) — memory definitions (FDK side)

   Defines the seven FDK memory blocks declared in drm_xhe_enc_ram.h with
   worst-case sizes (Option B) and the pool-dispenser allocator hooks handed
   to the vendored libxaac encoder.

   FDK-side TU: no vendored libxaac headers here (see drm_xhe_enc_pool.h).
   The vendored-struct sizes and worst-case compositions come from the
   vendor-side TU drm_xhe_vendor_sizes.cpp via the DrmXhe_Get*SizeMax()
   functions — configuration-independent, deterministic values.
   -------------------------------------------------------------------------- */

#include "drm_xhe_enc_ram.h"

/* ------------------------------------------------------------------------- */
/* FDK memory block definitions                                               */
/* ------------------------------------------------------------------------- */

C_ALLOC_MEM(Ram_drmXhe_Handle, DRM_XHE_ENCODER, 1)

C_AALLOC_MEM(Ram_drmXhe_ApiMem, UCHAR, DrmXhe_GetApiMemSizeMax() + DRMXHE_ALIGN_SLACK)
C_AALLOC_MEM(Ram_drmXhe_MemTabs, UCHAR, DrmXhe_GetMemTabsSizeMax() + DRMXHE_ALIGN_SLACK)
C_AALLOC_MEM(Ram_drmXhe_Persist, UCHAR, DrmXhe_GetPersistSizeMax() + DRMXHE_ALIGN_SLACK)
C_AALLOC_MEM(Ram_drmXhe_Scratch, UCHAR, DrmXhe_GetScratchSizeMax() + DRMXHE_ALIGN_SLACK)
C_AALLOC_MEM(Ram_drmXhe_InBuf, UCHAR, DRMXHE_INBUF_SIZE + DRMXHE_ALIGN_SLACK)
C_AALLOC_MEM(Ram_drmXhe_OutBuf, UCHAR, DRMXHE_OUTBUF_SIZE + DRMXHE_ALIGN_SLACK)

UINT drmXheEnc_GetRequiredMem(void) {
  return GetRequiredMemRam_drmXhe_Handle() + GetRequiredMemRam_drmXhe_ApiMem() +
         GetRequiredMemRam_drmXhe_MemTabs() + GetRequiredMemRam_drmXhe_Persist() +
         GetRequiredMemRam_drmXhe_Scratch() + GetRequiredMemRam_drmXhe_InBuf() +
         GetRequiredMemRam_drmXhe_OutBuf();
}

/* ------------------------------------------------------------------------- */
/* Pool dispenser (vendor-patched allocator hooks, pv_mem_ctx cookie)         */
/* ------------------------------------------------------------------------- */

void *DrmXhe_PoolMalloc(void *pvCtx, unsigned int size, unsigned int alignment) {
  DRMXHE_MEM_POOL *pool = (DRMXHE_MEM_POOL *)pvCtx;

  if (pool == NULL || pool->nextRegion >= (unsigned int)DRMXHE_MEM_N) {
    return NULL;
  }
  /* C_AALLOC_MEM blocks are ALIGNMENT_DEFAULT-aligned; the vendored encoder
     only ever requests 8-byte alignment. Reject anything stronger. */
  if (alignment > ALIGNMENT_DEFAULT) {
    return NULL;
  }
  /* The bound check: a vendored sizing change beyond our pinned worst case
     fails the create cleanly instead of corrupting memory. */
  if (size > pool->blockSize[pool->nextRegion]) {
    return NULL;
  }

  return (void *)pool->pBlock[pool->nextRegion++];
}

void DrmXhe_PoolFree(void *pvCtx, void *ptr) {
  /* Blocks are owned by the DRM_XHE_ENCODER handle and are returned via
     FreeRam_drmXhe_*() in drmXheEnc_Close(). The vendored encoder calls this
     hook from IXHEAACE_MEM_FREE (normal delete and cleanup-on-failed-create);
     both are deliberate no-ops here. */
  (void)pvCtx;
  (void)ptr;
}
