/* -----------------------------------------------------------------------------
   MPEG transport encode: DRM audio super frame writer for xHE-AAC.
   See tpenc_drm.h for the structural model and the PROVISIONAL bit-level
   layout markers.
   -------------------------------------------------------------------------- */

#include "tpenc_drm.h"

#include "FDK_crc.h"
#include "genericStds.h"

/* Payload capacity of one super frame given the number of frame borders. */
static UINT drmSf_Capacity(const DRM_SF_WRITER *hSf, const UINT borderCount) {
  return hSf->superFrameBytes - TPENC_DRM_HEADER_BYTES - 2 * borderCount;
}

/* Serialize the current (exactly full or explicitly padded) super frame.
   Layout per the project ruling — see tpenc_drm.h:
     [crc_8][reserved 0x00][payload][backwards directory]                  */
static TPENC_DRM_ERROR drmSf_Emit(DRM_SF_WRITER *hSf, HANDLE_FDK_BITSTREAM hBsOut) {
  const UINT payloadBytes = drmSf_Capacity(hSf, hSf->borderCount);
  UINT i;
  USHORT crc;
  INT crcReg;
  FDK_BITSTREAM bsCrc;

  if (FDKgetFreeBits(hBsOut) < (INT)(hSf->superFrameBytes * 8)) {
    return TPENC_DRM_BUFFER_TOO_SMALL;
  }

  /* The CRC byte precedes everything it protects (reserved header byte +
     directory), so pre-compute it over a scratch bitstream carrying exactly
     the protected control bits. Poly/config mirrored from tpdec_drm.cpp:
     0x001d, start 0xFF, final XOR 0xFF. */
  FDKinitBitStream(&bsCrc, hSf->crcScratch, sizeof(hSf->crcScratch), 0, BS_WRITER);
  FDKcrcReset(&hSf->crcInfo);
  crcReg = FDKcrcStartReg(&hSf->crcInfo, &bsCrc, 8 + 16 * hSf->borderCount);
  FDKwriteBits(&bsCrc, 0x00, 8); /* reserved header byte */
  for (i = hSf->borderCount; i > 0; i--) {
    FDKwriteBits(&bsCrc, hSf->borderPos[i - 1] & 0x0FFF, 12);
    FDKwriteBits(&bsCrc, hSf->borderCount & 0x0F, 4);
  }
  FDKsyncCache(&bsCrc);
  FDKcrcEndReg(&hSf->crcInfo, &bsCrc, crcReg);
  crc = (USHORT)(FDKcrcGetCRC(&hSf->crcInfo) ^ 0xFF);

  /* header: crc_8, then reserved 0x00 */
  FDKwriteBits(hBsOut, crc, 8);
  FDKwriteBits(hBsOut, 0x00, 8);

  /* payload section (zero padding beyond payloadFill was cleared by callers) */
  for (i = 0; i < payloadBytes; i++) {
    FDKwriteBits(hBsOut, hSf->payload[i], 8);
  }

  /* frame-border directory, growing backwards from the frame end: border
     i's entry occupies bytes [L-2*(i+1), L-2*i), so sequential emission
     writes the entries in REVERSE border order. Each entry:
     frameBorderIndex(12) | borderCountField(4) — see the header note on the
     count-repetition interpretation of the 4-bit field. */
  for (i = hSf->borderCount; i > 0; i--) {
    FDKwriteBits(hBsOut, hSf->borderPos[i - 1] & 0x0FFF, 12);
    FDKwriteBits(hBsOut, hSf->borderCount & 0x0F, 4);
  }

  FDKsyncCache(hBsOut);

  /* stream accounting: payloadBytes = AU data + zero padding */
  hSf->statFrames++;
  hSf->statBorders += hSf->borderCount;
  hSf->statAuBytes += hSf->payloadFill;
  hSf->statPadBytes += payloadBytes - hSf->payloadFill;

  /* start the next super frame */
  FDKmemclear(hSf->payload, sizeof(hSf->payload));
  hSf->payloadFill = 0;
  hSf->borderCount = 0;

  return TPENC_DRM_OK;
}

TPENC_DRM_ERROR drmSfWriter_Init(DRM_SF_WRITER *hSf, const UINT superFrameBytes) {
  if (hSf == NULL || superFrameBytes > TPENC_DRM_MAX_SF_BYTES ||
      superFrameBytes <= TPENC_DRM_HEADER_BYTES + 2) {
    return TPENC_DRM_INVALID_PARAM;
  }

  FDKmemclear(hSf, sizeof(*hSf));
  hSf->superFrameBytes = superFrameBytes;
  FDKcrcInit(&hSf->crcInfo, 0x001d, 0xFF, 8);

  return TPENC_DRM_OK;
}

TPENC_DRM_ERROR drmSfWriter_AddAu(DRM_SF_WRITER *hSf, const UCHAR *pAu,
                                  const UINT auBytes, HANDLE_FDK_BITSTREAM hBsOut,
                                  UINT *pnSfEmitted) {
  UINT remain = auBytes;
  const UCHAR *src = pAu;
  UINT nSf = 0;
  TPENC_DRM_ERROR err;

  if (hSf == NULL || pAu == NULL || hBsOut == NULL || pnSfEmitted == NULL) {
    return TPENC_DRM_INVALID_PARAM;
  }
  *pnSfEmitted = 0;

  /* The new AU needs a frame border (a directory entry) in the super frame
     it starts in. If the current one cannot take another border — either the
     4-bit count is exhausted or adding the 2-byte entry would collide with
     already-buffered payload — close it out first. When the frame is 1-2
     bytes short of full (space too small for an entry but nonzero), the AU's
     first bytes fill that slack and its border becomes a special
     TPENC_DRM_BORDER_PREV_* entry in the NEXT frame: mid-stream padding must
     never adhere to an AU (USAC AUs carry no length field). */
  if (hSf->borderCount >= TPENC_DRM_MAX_BORDERS ||
      drmSf_Capacity(hSf, hSf->borderCount + 1) <= hSf->payloadFill) {
    UINT slack = drmSf_Capacity(hSf, hSf->borderCount) - hSf->payloadFill;

    if (hSf->borderCount < TPENC_DRM_MAX_BORDERS && slack >= 1 && slack <= 2 &&
        remain > slack) {
      FDKmemcpy(&hSf->payload[hSf->payloadFill], src, slack);
      hSf->payloadFill += slack;
      src += slack;
      remain -= slack;
      hSf->pendingSpecialBorder = slack;
    }
    /* (borderCount == 15 with payload space left forces a padded emit — the
       residual, documented case; unreachable at this integration's AU sizes) */
    err = drmSf_Emit(hSf, hBsOut);
    if (err != TPENC_DRM_OK) {
      return err;
    }
    nSf++;
  }

  if (hSf->pendingSpecialBorder != 0) {
    hSf->borderPos[hSf->borderCount] = (hSf->pendingSpecialBorder == 1)
                                           ? TPENC_DRM_BORDER_PREV_1
                                           : TPENC_DRM_BORDER_PREV_2;
    hSf->pendingSpecialBorder = 0;
  } else {
    hSf->borderPos[hSf->borderCount] = TPENC_DRM_HEADER_BYTES + hSf->payloadFill;
  }
  hSf->borderCount++;

  while (remain > 0) {
    UINT cap = drmSf_Capacity(hSf, hSf->borderCount) - hSf->payloadFill;
    UINT chunk = (remain < cap) ? remain : cap;

    FDKmemcpy(&hSf->payload[hSf->payloadFill], src, chunk);
    hSf->payloadFill += chunk;
    src += chunk;
    remain -= chunk;

    if (hSf->payloadFill == drmSf_Capacity(hSf, hSf->borderCount)) {
      /* super frame exactly full; a continuation into the next super frame
         carries no border of its own */
      err = drmSf_Emit(hSf, hBsOut);
      if (err != TPENC_DRM_OK) {
        return err;
      }
      nSf++;
    }
  }

  *pnSfEmitted = nSf;
  return TPENC_DRM_OK;
}

TPENC_DRM_ERROR drmSfWriter_Flush(DRM_SF_WRITER *hSf, HANDLE_FDK_BITSTREAM hBsOut,
                                  UINT *pnSfEmitted) {
  TPENC_DRM_ERROR err;

  if (hSf == NULL || hBsOut == NULL || pnSfEmitted == NULL) {
    return TPENC_DRM_INVALID_PARAM;
  }
  *pnSfEmitted = 0;

  if (hSf->payloadFill == 0 && hSf->borderCount == 0) {
    return TPENC_DRM_OK;
  }

  /* payload beyond payloadFill is already zero (cleared at emit/init) */
  err = drmSf_Emit(hSf, hBsOut);
  if (err != TPENC_DRM_OK) {
    return err;
  }
  *pnSfEmitted = 1;

  return TPENC_DRM_OK;
}
