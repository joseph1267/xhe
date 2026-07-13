/* -----------------------------------------------------------------------------
   DRM xHE-AAC encoder library (libDRMxheEnc) — bit-writer drop-in

   Replaces the vendored libxaac_vendor/encoder/iusace_bitbuffer.c, which is
   EXCLUDED from every build list of this module. Interface and semantics are
   derived from that file (Apache-2.0, The Android Open Source Project /
   Ittiam Systems) and must remain bit-exact.

   WHY A DROP-IN AND NOT AN FDK_BITSTREAM WRAPPER: the arithmetic coder
   (iusace_arith_enc.c, static iusace_copy_bit_buf()) snapshots and rolls
   back writer state by directly copying the fields cnt_bits, ptr_write_next
   and write_position between ia_bit_buf_struct instances. Rollback is only
   correct if the writer's ENTIRE mutable state lives in exactly those
   fields, with byte-level read-modify-write into the shared buffer and no
   cache — which is incompatible with FDK_BITSTREAM's cached writer.
   Therefore:
     - all writer state stays in the (frozen) ia_bit_buf_struct fields;
     - writes are read-modify-write per byte, MSB-first, with the same
       circular wrap at ptr_bit_buf_end as the vendored original;
     - HANDLE_FDK_BITSTREAM ownership begins at the access-unit boundary
       (drm_xhe_enc.cpp) and covers all transport serialization (tpenc_drm).

   Vendor-side TU: vendored headers only (see drm_xhe_enc_pool.h).
   -------------------------------------------------------------------------- */

extern "C" {
#include "ixheaac_type_def.h"
#include "ixheaac_constants.h"
#include "iusace_cnst.h"
#include "iusace_bitbuffer.h"
}

#ifndef DRMXHE_MIN
#define DRMXHE_MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

extern "C" ia_bit_buf_struct *iusace_create_bit_buffer(ia_bit_buf_struct *it_bit_buf,
                                                       UWORD8 *ptr_bit_buf_base,
                                                       UWORD32 bit_buffer_size, WORD32 init) {
  it_bit_buf->ptr_bit_buf_base = ptr_bit_buf_base;
  it_bit_buf->ptr_bit_buf_end = ptr_bit_buf_base + bit_buffer_size - 1;
  it_bit_buf->ptr_read_next = ptr_bit_buf_base;
  it_bit_buf->ptr_write_next = ptr_bit_buf_base;

  if (init) {
    it_bit_buf->write_position = 7;
    it_bit_buf->read_position = 7;
    it_bit_buf->cnt_bits = 0;
    it_bit_buf->size = bit_buffer_size * 8;
  }

  return it_bit_buf;
}

extern "C" VOID iusace_reset_bit_buffer(ia_bit_buf_struct *it_bit_buf) {
  it_bit_buf->ptr_read_next = it_bit_buf->ptr_bit_buf_base;
  it_bit_buf->ptr_write_next = it_bit_buf->ptr_bit_buf_base;

  it_bit_buf->write_position = 7;
  it_bit_buf->read_position = 7;
  it_bit_buf->cnt_bits = 0;
}

extern "C" UWORD8 iusace_write_bits_buf(ia_bit_buf_struct *it_bit_buf, UWORD32 write_val,
                                        UWORD8 num_bits) {
  WORD8 bits_to_write;
  WORD32 write_position;
  UWORD8 *ptr_write_next;
  UWORD8 *ptr_bit_buf_end;
  UWORD8 *ptr_bit_buf_base;
  UWORD8 bits_written = num_bits;

  if (it_bit_buf) {
    it_bit_buf->cnt_bits += num_bits;

    write_position = it_bit_buf->write_position;
    ptr_write_next = it_bit_buf->ptr_write_next;
    ptr_bit_buf_end = it_bit_buf->ptr_bit_buf_end;
    ptr_bit_buf_base = it_bit_buf->ptr_bit_buf_base;

    while (num_bits) {
      UWORD8 tmp, msk;

      bits_to_write = (WORD8)DRMXHE_MIN(write_position + 1, (WORD32)num_bits);

      /* read-modify-write of the partial byte, MSB-first */
      tmp = (UWORD8)(write_val << (32 - num_bits) >> (32 - bits_to_write)
                                                         << (write_position + 1 - bits_to_write));
      msk = (UWORD8)~(((1 << bits_to_write) - 1) << (write_position + 1 - bits_to_write));

      *ptr_write_next &= msk;
      *ptr_write_next |= tmp;

      write_position -= bits_to_write;
      num_bits -= bits_to_write;

      if (write_position < 0) {
        write_position += 8;
        ptr_write_next++;

        if (ptr_write_next > ptr_bit_buf_end) {
          ptr_write_next = ptr_bit_buf_base;
        }
      }
    }

    it_bit_buf->write_position = write_position;
    it_bit_buf->ptr_write_next = ptr_write_next;
  }

  return bits_written;
}

extern "C" WORD32 iusace_write_escape_value(ia_bit_buf_struct *pstr_it_bit_buff, UWORD32 value,
                                            UWORD8 no_bits1, UWORD8 no_bits2, UWORD8 no_bits3) {
  WORD32 bit_cnt = 0;
  UWORD32 esc_val = 0;
  UWORD32 max_val1 = (1u << no_bits1) - 1;
  UWORD32 max_val2 = (1u << no_bits2) - 1;
  UWORD32 max_val3 = (1u << no_bits3) - 1;

  esc_val = DRMXHE_MIN(value, max_val1);
  bit_cnt += iusace_write_bits_buf(pstr_it_bit_buff, esc_val, no_bits1);

  if (esc_val == max_val1) {
    value = value - esc_val;

    esc_val = DRMXHE_MIN(value, max_val2);
    bit_cnt += iusace_write_bits_buf(pstr_it_bit_buff, esc_val, no_bits2);

    if (esc_val == max_val2) {
      value = value - esc_val;

      esc_val = DRMXHE_MIN(value, max_val3);
      bit_cnt += iusace_write_bits_buf(pstr_it_bit_buff, esc_val, no_bits3);
    }
  }

  return bit_cnt;
}
