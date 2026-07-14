/* -----------------------------------------------------------------------------
   MPEG transport encode: DRM (Digital Radio Mondiale) audio super frame
   writer for xHE-AAC (ETSI ES 201 980).

   Structural model (ETSI ES 201 980, xHE-AAC audio super frame):
     - fixed super-frame length in bytes, derived from the audio channel
       bit rate and the super-frame duration (400 ms):
         L = bitRate * superFrameMs / 8000
     - a 2-byte header,
     - a payload section of consecutive variable-length USAC access units;
       an AU may straddle super-frame boundaries (bit-reservoir behavior),
     - a frame-border directory at the tail: one 2-byte entry per AU that
       STARTS inside this super frame (max 15, 4-bit count), and
     - CRC-8 protection with the polynomial/config mirrored from fdk-aac's
       DRM transport decode side (tpdec_drm.cpp: poly 0x001d, start 0xFF,
       final XOR 0xFF).

   Bit-level layout (per project ruling, 2026-07; supersedes the earlier
   provisional layout):
     byte 0      : crc_8 — CRC-8 over the reserved header byte plus the
                   frame-border directory (the control information; coverage
                   is this module's documented choice, as the ruling fixed
                   field placement but not CRC extent)
     byte 1      : reserved, 0x00
     bytes 2..P-1: AU payload, P = L - 2*frameBorderCount
     bytes P..L-1: frame-border directory, growing BACKWARDS from the end
                   of the super frame: the entry for border i (0-based, in
                   AU order) occupies bytes [L-2*(i+1), L-2*i). Each entry:
                   frameBorderIndex(12) | borderCountField(4).

   NOTE on borderCountField: the ruling describes this 4-bit field as a
   compressed AU-length shorthand, but 4 bits cannot encode real AU sizes
   (hundreds to thousands of bytes) and, with the header carrying no border
   count, a receiver could not even locate the directory without a count
   somewhere. This writer therefore stores the super frame's frame-border
   COUNT in the field (the count-repetition reading); a receiver recovers
   the directory size from the last 2 bytes of the frame. A super frame
   containing only the continuation of a straddling AU has zero borders and
   no directory (this genuinely occurs with 100 ms frames, where a transient
   AU can exceed one frame's payload); receivers disambiguate by scanning
   candidate counts k = 0..15 for the unique k whose count-repetition fields
   AND header CRC (which covers the directory, and validates k = 0 against
   the lone reserved byte) are consistent — see drmFindBorderCount() in
   drm_xhe_dec_tool.cpp.

   All serialization goes through HANDLE_FDK_BITSTREAM (FDKwriteBits) and
   FDKcrc, per the integration's bitstream rule.
   -------------------------------------------------------------------------- */

#ifndef TPENC_DRM_H
#define TPENC_DRM_H

#include "machine_type.h"
#include "FDK_bitstream.h"
#include "FDK_crc.h"

#define TPENC_DRM_MAX_SF_BYTES (4093) /* 12-bit border index, minus the two
                                         reserved special values below */
#define TPENC_DRM_MAX_BORDERS (15)    /* 4-bit frame-border count */
#define TPENC_DRM_HEADER_BYTES (2)

/* Special frame-border index values: the AU whose border this is STARTED in
   the last 1 / 2 byte(s) of the PREVIOUS super frame's payload. Used when a
   super frame ends with 1-2 bytes of space — too small for another
   directory entry — so that no padding ever adheres to an AU mid-stream
   (padding is fatal: USAC AUs carry no length field, and strict decoders
   reject trailing bytes). Such an entry is always border 0 of its frame. */
#define TPENC_DRM_BORDER_PREV_1 (0xFFF) /* AU began 1 byte before frame end */
#define TPENC_DRM_BORDER_PREV_2 (0xFFE) /* AU began 2 bytes before frame end */

typedef enum {
  TPENC_DRM_OK = 0,
  TPENC_DRM_INVALID_PARAM = 1,
  TPENC_DRM_TOO_MANY_BORDERS = 2, /* AU flow forced >15 borders per SF */
  TPENC_DRM_BUFFER_TOO_SMALL = 3  /* output bitstream cannot hold the SF */
} TPENC_DRM_ERROR;

/*
 * DRM super-frame writer state. Caller-owned (no hidden allocation; this
 * module performs no memory management of its own).
 */
typedef struct {
  UINT superFrameBytes; /* fixed L */
  UINT payloadFill;     /* bytes of AU data in the current super frame */
  UINT borderCount;     /* AUs started in the current super frame */
  UINT borderPos[TPENC_DRM_MAX_BORDERS]; /* absolute offsets within the SF */
  UCHAR payload[TPENC_DRM_MAX_SF_BYTES]; /* payload accumulation area */
  UCHAR crcScratch[2 + 2 * TPENC_DRM_MAX_BORDERS]; /* CRC pre-computation */
  UINT pendingSpecialBorder; /* 1 or 2: the AU about to be recorded started
                                that many bytes before the end of the super
                                frame just emitted (TPENC_DRM_BORDER_PREV_*) */
  FDK_CRCINFO crcInfo;
} DRM_SF_WRITER;

/**
 * Initialize a writer for a fixed super-frame length.
 * superFrameBytes = bitRate * superFrameMs / 8000, must be in
 * (TPENC_DRM_HEADER_BYTES + 2, TPENC_DRM_MAX_SF_BYTES].
 */
TPENC_DRM_ERROR drmSfWriter_Init(DRM_SF_WRITER *hSf, const UINT superFrameBytes);

/**
 * Append one access unit. Whenever the current super frame becomes exactly
 * full, it is serialized into hBsOut (possibly multiple times for an AU
 * larger than one super frame). *pnSfEmitted returns how many complete
 * super frames were written by this call (0..n).
 */
TPENC_DRM_ERROR drmSfWriter_AddAu(DRM_SF_WRITER *hSf, const UCHAR *pAu,
                                  const UINT auBytes, HANDLE_FDK_BITSTREAM hBsOut,
                                  UINT *pnSfEmitted);

/**
 * End of stream: if the current super frame holds any data, zero-pad the
 * payload and emit it. Note: mid-payload zero padding is not distinguishable
 * from AU data by a receiver (the final AU's length is delimited only by the
 * next super frame's first border); real DRM encoders avoid this via
 * bit-reservoir rate control. Provided for stream finalization and testing.
 */
TPENC_DRM_ERROR drmSfWriter_Flush(DRM_SF_WRITER *hSf, HANDLE_FDK_BITSTREAM hBsOut,
                                  UINT *pnSfEmitted);

#endif /* TPENC_DRM_H */
