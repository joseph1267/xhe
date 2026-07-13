/* -----------------------------------------------------------------------------
   DRM xHE-AAC encoder library (libDRMxheEnc) — vendored sizing (vendor side)

   Vendor-side TU: includes ONLY the vendored libxaac header chain (replicated
   from libxaac_vendor/encoder/ixheaace_api.c) plus the neutral pool header —
   never fdk-aac headers (macro collisions, see drm_xhe_enc_pool.h).

   Provides:
     - static_asserts pinning every constant mirrored in drm_xhe_enc_pool.h
       to its authoritative vendored definition;
     - the worst-case region sizes, composed from real vendored sizeof()s and
       libxaac's exported sizing functions evaluated at the pinned worst-case
       configuration (2 channels, ccfl 1024, eSBR 4:1 + harmonic, DRC on,
       TNS on, delay adjustment on) — configuration-independent values.

   The mirrored FORMULAS (of libxaac functions that are static and therefore
   uncallable: iusace_calc_pers_buf_sizes, ia_enhaacplus_enc_sizeof_delay_
   buffer, the fill_mem_tabs region terms) are the only thing maintained by
   hand; the pool dispenser's size check at create time is the final guard
   should a vendor refresh ever change their shape.
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
#include "impd_drc_uni_drc_eq.h"
#include "impd_drc_uni_drc_filter_bank.h"
#include "impd_drc_gain_enc.h"
#include "impd_drc_struct_def.h"
#include "impd_drc_enc.h"
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
#include "iusace_block_switch_const.h"
#include "iusace_block_switch_struct_def.h"
#include "iusace_cnst.h"
#include "iusace_tns_usac.h"
#include "iusace_psy_mod.h"
#include "iusace_config.h"
#include "iusace_arith_enc.h"
#include "ixheaace_version_number.h"
#include "ixheaace_adjust_threshold_data.h"
#include "ixheaace_dynamic_bits.h"
#include "ixheaace_qc_data.h"
#include "ixheaace_channel_map.h"
#include "ixheaace_block_switch.h"
#include "ixheaace_psy_data.h"
#include "ixheaace_interface.h"
#include "ixheaace_write_bitstream.h"
#include "ixheaace_psy_configuration.h"
#include "ixheaace_psy_mod.h"
#include "iusace_fd_qc_util.h"
#include "iusace_fd_quant.h"
#include "iusace_ms.h"
#include "iusace_signal_classifier.h"
#include "ixheaace_config.h"
#include "ixheaace_asc_write.h"
#include "iusace_main.h"
#include "ixheaace_stereo_preproc.h"
#include "ixheaace_enc_main.h"
#include "ixheaace_qc_util.h"
#include "ixheaace_mps_common_fix.h"
#include "ixheaace_mps_defines.h"
#include "ixheaace_mps_common_define.h"
#include "ixheaace_mps_struct_def.h"
#include "ixheaace_mps_sac_polyphase.h"
#include "ixheaace_mps_sac_hybfilter.h"
#include "ixheaace_mps_bitstream.h"
#include "ixheaace_mps_spatial_bitstream.h"
#include "ixheaace_mps_buf.h"
#include "ixheaace_mps_lib.h"
#include "ixheaace_mps_main_structure.h"
#include "ixheaace_mps_onset_detect.h"
#include "ixheaace_mps_param_extract.h"
#include "ixheaace_mps_static_gain.h"
#include "ixheaace_mps_filter.h"
#include "ixheaace_mps_delay.h"
#include "ixheaace_mps_dmx_tdom_enh.h"
#include "ixheaace_mps_tools_rom.h"
#include "ixheaace_mps_qmf.h"
#include "ixheaace_mps_tree.h"
#include "ixheaace_mps_frame_windowing.h"
#include "ixheaace_mps_structure.h"
#include "ixheaace_mps_memory.h"
#include "ixheaace_mps_enc.h"
#include "ixheaace_struct_def.h"
#include "ixheaace_api_defs.h"
#include "ixheaace_write_adts_adif.h"
#include "ixheaace_loudness_measurement.h"
#include "iusace_psy_utils.h"
} /* extern "C" */

#include "drm_xhe_enc_pool.h"

/* ------------------------------------------------------------------------- */
/* Type-equivalence and mirrored-constant cross-checks                        */
/* ------------------------------------------------------------------------- */

static_assert(sizeof(UWORD32) == sizeof(unsigned int) && sizeof(pVOID) == sizeof(void *),
              "pool interface type equivalence violated");
static_assert(DRMXHE_USAC_SCR_SIZE == USACE_MAX_SCR_SIZE,
              "DRMXHE_USAC_SCR_SIZE out of sync with vendored USACE_MAX_SCR_SIZE");
static_assert(DRMXHE_MAX_CCFL == MAX_FRAME_LEN,
              "DRMXHE_MAX_CCFL out of sync with vendored MAX_FRAME_LEN");
static_assert(DRMXHE_MAX_CH == MAX_CHANNELS,
              "DRMXHE_MAX_CH out of sync with vendored MAX_CHANNELS (stereo scope)");
static_assert(DRMXHE_OUTBUF_SIZE ==
                  ((MAX_PREROLL_FRAMES + 1) * (MAX_CHANNEL_BITS / 8) * DRMXHE_MAX_CH +
                   MAX_PREROLL_CONFIG_SIZE + MAX_DRC_CONFIG_SIZE_EXPECTED),
              "DRMXHE_OUTBUF_SIZE out of sync with vendored OUTPUT sizing terms");
static_assert(sizeof(ia_drc_input_config) <= DRMXHE_DRC_CFG_MEM_SIZE,
              "DRMXHE_DRC_CFG_MEM_SIZE too small for vendored ia_drc_input_config");
static_assert(sizeof(ixheaace_input_config) <= DRMXHE_INCFG_MEM_SIZE,
              "DRMXHE_INCFG_MEM_SIZE too small for vendored ixheaace_input_config");
static_assert(sizeof(ixheaace_output_config) <= DRMXHE_OUTCFG_MEM_SIZE,
              "DRMXHE_OUTCFG_MEM_SIZE too small for vendored ixheaace_output_config");

#define DRMXHE_ALIGN8(x) IXHEAAC_GET_SIZE_ALIGNED((x), BYTE_ALIGN_8)

/* ------------------------------------------------------------------------- */
/* Worst-case region sizes                                                    */
/* ------------------------------------------------------------------------- */

unsigned int DrmXhe_GetApiMemSizeMax(void) {
  /* mirror of ixheaace_allocate(): ui_api_size (+8 slack added FDK-side) */
  return (unsigned int)sizeof(ixheaace_api_struct);
}

unsigned int DrmXhe_GetMemTabsSizeMax(void) {
  /* mirror of ixheaace_allocate(): ui_proc_mem_tabs_size */
  return (unsigned int)((sizeof(ixheaace_mem_info_struct) + sizeof(pVOID *)) * 4);
}

/* Mirror of the static iusace_calc_pers_buf_sizes() (vendored ixheaace_api.c)
   at the pinned worst case: 2 channels, ccfl 1024, drc_frame_size = ccfl,
   delay adjustment on, TNS on. Real sizeof()s track struct growth
   automatically; only the formula is mirrored. */
static unsigned int DrmXhe_UsacPersBufSizesMax(void) {
  const unsigned int ch = DRMXHE_MAX_CH;
  const unsigned int ccfl = DRMXHE_MAX_CCFL;
  const unsigned int drcFrame = DRMXHE_MAX_CCFL;
  unsigned int s = 0;

  s += 4 * DRMXHE_ALIGN8(ch * sizeof(FLOAT32 *));
  s += DRMXHE_ALIGN8(2 * ccfl * sizeof(FLOAT32)) * ch;
  s += DRMXHE_ALIGN8(2 * drcFrame * sizeof(FLOAT32)) * ch;
  /* use_delay_adjustment == 1 */
  s += DRMXHE_ALIGN8(((CC_DELAY_ADJUSTMENT * ccfl) / FRAME_LEN_1024) * sizeof(FLOAT32)) * ch;
  s += DRMXHE_ALIGN8(((CC_DELAY_ADJUSTMENT * drcFrame) / FRAME_LEN_1024) * sizeof(FLOAT32)) * ch;
  s += DRMXHE_ALIGN8(2 * ccfl * sizeof(FLOAT64)) * ch;
  s += DRMXHE_ALIGN8(ccfl * sizeof(FLOAT64)) * ch;
  s += DRMXHE_ALIGN8(2 * ccfl * sizeof(FLOAT64)) * ch;
  s += DRMXHE_ALIGN8(3 * ccfl * sizeof(FLOAT64)) * ch;
  /* tns_select != 0 */
  s += DRMXHE_ALIGN8(sizeof(ia_tns_info)) * ch;
  s += DRMXHE_ALIGN8(sizeof(ia_usac_td_encoder_struct)) * ch;

  return s;
}

/* Mirror of the static ia_enhaacplus_enc_sizeof_delay_buffer() (vendored
   ixheaace_api.c), AOT_USAC branch, maximized over the DRM-reachable
   resampler indices (1: none, 2: 8:3, 3: 2:1, 4: 4:1). */
static unsigned int DrmXhe_UsacDelayBufSizeMax(void) {
  const unsigned int delayBufSize = sizeof(FLOAT32);
  unsigned int sizeMax = 0;

  for (int resampIdx = 1; resampIdx <= 4; resampIdx++) {
    unsigned int downsampleFac = (resampIdx > 1) ? (unsigned int)(resampIdx / 2) : 1;
    unsigned int maxDown, maxUp;
    if (resampIdx == 2) { /* 8:3 */
      maxDown = MAXIMUM_DS_8_1_FILTER_DELAY;
      maxUp = MAXIMUM_DS_1_3_FILTER_DELAY;
    } else if (resampIdx == 4) { /* 4:1 */
      maxDown = MAXIMUM_DS_4_1_FILTER_DELAY;
      maxUp = 0;
    } else { /* none / 2:1 */
      maxDown = MAXIMUM_DS_2_1_FILTER_DELAY;
      maxUp = 0;
    }
    unsigned int size =
        (FRAME_LEN_1024 * (1u << downsampleFac) + maxUp + maxDown + INPUT_DELAY_LC) *
        IXHEAACE_MAX_CH_IN_BS_ELE * delayBufSize;
    if (size > sizeMax) {
      sizeMax = size;
    }
  }

  return sizeMax;
}

unsigned int DrmXhe_GetPersistSizeMax(void) {
  unsigned int s = 0;

  /* mirror of ixheaace_fill_mem_tabs(), USAC branch, sbr_enable = 1,
     use_mps = 0, num_bs_elements = 1, worst of mono/stereo */
  s += DRMXHE_ALIGN8(sizeof(ixheaace_state_struct));
  s += DrmXhe_UsacPersBufSizesMax();
  s += (unsigned int)ixheaace_sbr_enc_pers_size(DRMXHE_MAX_CH, 0, /* harmonic */ 1);
  /* input delay lines: 2 * delay buffer */
  s += DRMXHE_ALIGN8(2 * DrmXhe_UsacDelayBufSizeMax());
  /* eSBR downsampler buffer, fac_downsample = 2 (4:1) */
  s += DRMXHE_ALIGN8((MAX_FRAME_LEN * (1 << 2) + MAX_DS_8_1_FILTER_DELAY + INPUT_DELAY) *
                     MAX_CHANNELS * sizeof(UWORD32));
  /* mono time_signal staging (taken unconditionally: worst of mono/stereo) */
  s += DRMXHE_ALIGN8((MAX_INPUT_SAMPLES) * sizeof(FLOAT32));

  return s;
}

unsigned int DrmXhe_GetScratchSizeMax(void) {
  const unsigned int usacScratch = DRMXHE_ALIGN8(DRMXHE_USAC_SCR_SIZE);
  const unsigned int sbrScratch =
      (unsigned int)ixheaace_sbr_enc_scr_size() + (unsigned int)ixheaace_resampler_scr_size();

  return (usacScratch > sbrScratch) ? usacScratch : sbrScratch;
}
