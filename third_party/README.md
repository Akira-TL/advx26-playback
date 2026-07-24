# Playback third-party dependencies

## tinyh264

- Upstream: `udevbe/tinyh264`
- Pinned revision: `524a702b6d98f8b9271238fdda9a064a1e6861e7` (`v0.0.7`)
- License: Apache License 2.0; see `tinyh264/LICENSE`
- Scope: Baseline/Constrained Baseline H.264 software decode to planar YUV420.

The upstream submodule is kept unmodified. During CMake configuration, its `native/` directory is copied into the build tree and `patches/tinyh264-embedded.patch` is applied there. The patch:

1. redirects decoder allocations through the playback PSRAM port;
2. replaces signed left shifts that are undefined in C with equivalent multiplication;
3. initializes conditional working variables reported by strict compilers;
4. removes dead variables without changing decode behavior.

The patched decoder was validated against FFmpeg with a 480 x 320, 10 fps, no-B-frame Constrained Baseline stream. Normal decoding and decoder reset at an IDR produced byte-identical YUV420 output.
