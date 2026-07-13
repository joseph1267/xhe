/* -----------------------------------------------------------------------------
   DRM xHE-AAC encoder library (libDRMxheEnc) — neutral pool interface

   This header is deliberately NEUTRAL: it includes neither fdk-aac headers
   nor vendored libxaac headers, and uses only plain C types. Reason: the two
   header universes cannot coexist in one translation unit — vendored
   ixheaace_api.h defines AOT_PS/AOT_USAC/AOT_SBR/... and ixheaace_qc_data.h
   defines ID_SCE/ID_CPE/... as MACROS, which destroy the identically-named
   enumerators in fdk-aac's FDK_audio.h (AUDIO_OBJECT_TYPE, MP4_ELEMENT_ID).

   The module is therefore split into two kinds of translation units that
   share only this header:
     - FDK-side TUs   (drm_xhe_enc_ram.cpp, later drm_xhe_enc.cpp):
                       include fdk-aac headers, never vendored ones.
     - vendor-side TUs (drm_xhe_vendor_sizes.cpp, later the vendor glue):
                       include the vendored libxaac chain, never fdk-aac ones.

   Type equivalences used here (checked in drm_xhe_vendor_sizes.cpp):
   libxaac UWORD32 == unsigned int, pVOID == void*, and fdk-aac UINT ==
   unsigned int, UCHAR == unsigned char.
   -------------------------------------------------------------------------- */

#ifndef DRM_XHE_ENC_POOL_H
#define DRM_XHE_ENC_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Pinned worst-case scope (DRM: mono/stereo, eSBR <= 4:1, DRC on)            */
/* ------------------------------------------------------------------------- */

#define DRMXHE_MAX_CH (2)
#define DRMXHE_MAX_CCFL (1024)
#define DRMXHE_MAX_ESBR_FAC (4)
#define DRMXHE_MAX_PCM_BYTES (4)

/* Mirror of vendored USACE_MAX_SCR_SIZE (iusace_cnst.h);
   iusace_scratch_size() is static inside ixheaace_api.c.
   Cross-checked by static_assert in drm_xhe_vendor_sizes.cpp. */
#define DRMXHE_USAC_SCR_SIZE (733836)

/* OUTPUT region worst case, mirror of ixheaace_fill_mem_tabs() USAC branch:
   (MAX_PREROLL_FRAMES+1) * (MAX_CHANNEL_BITS/8) * ch + MAX_PREROLL_CONFIG_SIZE
   + MAX_DRC_CONFIG_SIZE_EXPECTED. Cross-checked in drm_xhe_vendor_sizes.cpp. */
#define DRMXHE_OUTBUF_SIZE \
  ((3 + 1) * (6144 / 8) * DRMXHE_MAX_CH + 1024 + 14336)

/* INPUT staging worst case: ccfl * channels * bytes/sample * eSBR ratio. */
#define DRMXHE_INBUF_SIZE \
  (DRMXHE_MAX_CCFL * DRMXHE_MAX_CH * DRMXHE_MAX_PCM_BYTES * DRMXHE_MAX_ESBR_FAC)

/* Opaque-storage bounds for vendored structs embedded (by size, not by type)
   in the FDK-side handle. Each is cross-checked against the real sizeof by a
   static_assert in drm_xhe_vendor_sizes.cpp. */
#define DRMXHE_DRC_CFG_MEM_SIZE \
  (1600 * 1024) /* >= sizeof(ia_drc_input_config); the vendored uniDrc      \
                   config carries max-sized metadata arrays (~1.47 MiB      \
                   measured with MSVC x64) */
#define DRMXHE_INCFG_MEM_SIZE (1024)    /* >= sizeof(ixheaace_input_config)   */
#define DRMXHE_OUTCFG_MEM_SIZE (1024)   /* >= sizeof(ixheaace_output_config)  */

/* libxaac's manual alignment fixups request size + alignment (8) bytes;
   headroom added to every dispensed block. */
#define DRMXHE_ALIGN_SLACK (8)

/* ------------------------------------------------------------------------- */
/* The six libxaac allocations, in exact ixheaace_create() request order      */
/* (ixheaace_allocate(): API struct, mem-tab metadata; then                   */
/* ixheaace_alloc_and_assign_mem(): PERSIST, SCRATCH, INPUT, OUTPUT).         */
/* ------------------------------------------------------------------------- */
typedef enum {
  DRMXHE_MEM_API = 0,
  DRMXHE_MEM_MEMTABS = 1,
  DRMXHE_MEM_PERSIST = 2,
  DRMXHE_MEM_SCRATCH = 3,
  DRMXHE_MEM_INPUT = 4,
  DRMXHE_MEM_OUTPUT = 5,
  DRMXHE_MEM_N = 6
} DRMXHE_MEM_REGION;

/*
 * Pre-allocated block pool dispensed to the vendored encoder through the
 * (vendor-patched, pv_mem_ctx-carrying) allocator hooks. nextRegion is the
 * dispenser cursor; reset to DRMXHE_MEM_API immediately before
 * ixheaace_create(), and must equal DRMXHE_MEM_N when create returns.
 */
typedef struct {
  unsigned char *pBlock[DRMXHE_MEM_N];  /* owned by the handle (GetRam_*)  */
  unsigned int blockSize[DRMXHE_MEM_N]; /* pinned worst-case byte sizes    */
  unsigned int nextRegion;              /* dispenser cursor                */
} DRMXHE_MEM_POOL;

/* ------------------------------------------------------------------------- */
/* Cross-universe functions                                                   */
/* ------------------------------------------------------------------------- */

/* Implemented FDK-side (drm_xhe_enc_ram.cpp): pool dispenser, installed into
   the vendored malloc_xheaace/free_xheaace hooks with pvCtx = the owning
   handle's DRMXHE_MEM_POOL*. PoolFree is a deliberate no-op (blocks are
   returned via FreeRam_* in drmXheEnc_Close(); also covers libxaac's
   cleanup-on-failed-create). */
void *DrmXhe_PoolMalloc(void *pvCtx, unsigned int size, unsigned int alignment);
void DrmXhe_PoolFree(void *pvCtx, void *ptr);

/* Implemented vendor-side (drm_xhe_vendor_sizes.cpp): worst-case region
   sizes composed from real vendored sizeof()s and libxaac's exported sizing
   functions pinned at the worst-case configuration — configuration-
   independent, deterministic values. */
unsigned int DrmXhe_GetApiMemSizeMax(void);
unsigned int DrmXhe_GetMemTabsSizeMax(void);
unsigned int DrmXhe_GetPersistSizeMax(void);
unsigned int DrmXhe_GetScratchSizeMax(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DRM_XHE_ENC_POOL_H */
