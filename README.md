# fdk-aac + xHE-AAC (MPEG-D USAC) encoder

This is [mstorsjo/fdk-aac](https://github.com/mstorsjo/fdk-aac) (the
Fraunhofer FDK AAC Codec Library for Android) extended with an
**xHE-AAC / MPEG-D USAC encoder path** (`AUDIO_OBJECT_TYPE` 42).

Upstream FDK-AAC already *decodes* USAC/xHE-AAC (`libAACdec/src/usacdec_*`)
but its encoder (`libAACenc`) only goes up to HE-AACv2. This fork adds the
missing encoder side.

## How it's implemented

USAC's core coder (switched MDCT/ACELP/TCX with arithmetic-coded spectral
data), eSBR and MPEG-D DRC are substantial, independently-verified DSP
algorithms. Rather than re-implement them from scratch inside FDK's own
`libAACenc` data structures (high risk of subtle non-compliance with no way
to validate bit-exactness here), this fork vendors the real, working USAC
encoder from [ittiam-systems/libxaac](https://github.com/ittiam-systems/libxaac)
(Apache-2.0) into `third_party/libxaac/` and bridges it to FDK's existing
public API:

- `third_party/libxaac/` — vendored Ittiam `encoder/` + `common/` sources,
  unmodified except for build integration.
- `libAACenc/src/usacenc_adapter.{h,cpp}` — the only translation unit that
  includes both FDK and Ittiam headers; translates between FDK's
  `aacEncOpen`/`aacEncoder_SetParam`/`aacEncEncode`/`aacEncClose` API and
  Ittiam's `ixheaace_create`/`ixheaace_process`/`ixheaace_delete` API.
- `libAACenc/src/aacenc_lib.cpp` — when `AACENC_AOT` is set to 42, the
  classic AAC/SBR/PS/transport init and per-frame encode path is bypassed
  and delegated entirely to the adapter (mirrors how the existing decoder
  already branches into `usacdec_*` for USAC).

From a caller's point of view nothing changes: open a `HANDLE_AACENCODER`,
set `AACENC_AOT` to `42`, and call `aacEncEncode()` per frame as usual.

## DRM (Digital Radio Mondiale) preset

A new `AACENC_USAC_DRM_PROFILE` parameter (only meaningful with
`AACENC_AOT` = 42) applies a preset typical for DRM services (ETSI ES 201
980): 12/24 kHz core sample rate, mono for the low profile. See
`libAACenc/src/usacenc_adapter.h` for details, **including an important
caveat**: the vendored Ittiam encoder currently only supports two fixed USAC
bit rate operating points, 64000 and 96000 bps (any other requested bit
rate is silently clamped to the nearer of the two — this is documented in
`third_party/libxaac/README_enc.md` as a current upstream limitation, not
something introduced by this integration). DRM services typically want much
lower bit rates than that, so **true low-bitrate DRM xHE-AAC is not yet
achievable through this integration** until the vendored encoder itself
gains more operating points. The DRM transport/multiplex layer itself
(SDC, audio super-frames) is also out of scope here — this only produces
raw USAC access units / an ADTS-like elementary stream.

## Trying it

```sh
mkdir build && cd build
cmake .. -DBUILD_PROGRAMS=ON
cmake --build .
./aac-enc -t 42 -r 64000 in.wav out.aac        # xHE-AAC, generic
./aac-enc -t 42 -r 64000 -d 1 in.wav out.aac   # xHE-AAC, DRM "low" preset
```

## Known limitations of this integration

- Only two USAC bit rate operating points are currently supported (64000 /
  96000 bps) — see above.
- `aacEncInfo()`'s `confBuf`/`confSize` (out-of-band AudioSpecificConfig,
  used by some muxers) is not yet populated for the USAC path; the
  AudioSpecificConfig is instead embedded once at the start of the raw
  elementary stream produced by the first `aacEncEncode()` call.
- eSBR is disabled by default (`esbr_flag = 0`) to avoid a ccfl_idx/eSBR
  mismatch that desyncs the requested bit rate from the actual encoded
  rate; wiring an eSBR-enabled preset with a correctly paired `ccfl_idx`
  is a follow-up.
- USAC channel count is limited to mono/stereo (matches the vendored
  encoder's current channel support).
- MPEG-D DRC (loudness/dynamic range metadata) is wired through but
  disabled by default (`use_drc_element = 0`); enabling it end-to-end
  through the FDK API is a follow-up.

See `libAACenc/src/usacenc_adapter.cpp` / `.h` for implementation detail
and `test/usac_smoke_test.cpp` for a minimal end-to-end verification
program (encodes a synthetic sine wave through the public API and checks
that valid bitstream bytes come out).

---

Original upstream README content follows below (fdk-aac itself does not
ship a README.md; see `documentation/` and `NOTICE` for licensing).
