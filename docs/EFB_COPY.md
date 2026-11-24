# EFB copies (BP 0x52)

`gcrecomp_gp` handles both kinds of EFB copy. Pure conversion and tiling live in `src/gp/efb_copy.{h,cpp}`
(unit test: `tests/efb_copy_test.cpp`, run with `ctest`).

| Register | Use |
|---|---|
| BP 0x49 / 0x4A | source rectangle (x, y; width-1, height-1) |
| BP 0x4B | destination address in main memory (`value << 5`), texture copies only |
| BP 0x4D | destination stride in 32-byte units, per **row of tiles** |
| BP 0x4E | Y scale, `256 / scale` (9 bits); applied only when command bit 10 is set |
| BP 0x43 | pixel format; `Z24` (3) turns the copy into a depth copy |
| BP 0x53 / 0x54 | vertical filter taps (see below) |

Copy command bits:

| Bits | Meaning | Handled |
|---|---|---|
| 3, 4-6 | destination format (bit 3 is the high bit of the nibble) | yes, see below |
| 9 | half scale (2x2 box filter) | yes |
| 10 | apply the Y scale from BP 0x4E (nearest row) | yes |
| 11 | clear the EFB afterwards | yes |
| 14 | 1 = copy to the XFB (present a frame), 0 = copy to a texture in memory | yes |
| 15 | intensity formats take luma (Y = 0.257R + 0.504G + 0.098B + 16) instead of R | yes |
| 0, 1 | clamp top / bottom: the vertical filter repeats the first / last row instead of reading the EFB beyond the rectangle | yes, see "Vertical copy filter" |
| 2, 7-8, 12-13, 16 | YUV, gamma, frame-to-field, auto conversion | ignored (`diag.txt` of a bundle lists the ones a frame used) |

Color formats: I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, A8, R8, G8, B8, RG8, GB8. Depth formats (Z24 mode):
Z4, Z8, Z16, Z24X8, Z8M, Z8L, Z16L. Output uses the console's tile layouts, so the existing texture path
decodes it. The byte order of RG8, GB8, Z16 and Z16L and the Z24X8 alpha byte (0xFF) follow the usual
conventions but are not verified against hardware.

Texture copies read the EFB back from the GPU on every copy. The texture cache is keyed on a hash of the guest
bytes (kept while the write stamps of `Memory` say the pages were not stored to; the copy stamps its destination), so entries overlapping the destination are re-decoded on next use. A
destination outside RAM is ignored with a message on stderr. Texture copies do not present a frame.

## Vertical copy filter (BP 0x53 / 0x54)

Every colour copy, to a texture or to the XFB, goes through a vertical filter. BP 0x53 holds the taps `c0..c3` and BP 0x54
`c4..c6`, 6 bits each from bit 0. The SDK's `GXSetCopyFilter(aa, sample_pattern, vf, vfilter)` writes both for every copy: with
`vf` false (or a render mode without deflicker) the taps are `0 0 21 22 21 0 0`, the render modes with deflicker use
`8 8 10 12 10 8 8`. There is no enable bit in BP 0x52, 0x53 or 0x54.

The seven taps are weights, in 64ths, of the sample rows of a three-line window: `c0 + c1` for the line above, `c2 + c3 + c4` for
the line itself, `c5 + c6` for the line below (the layout of the 12-sample anti-aliasing pattern; without anti-aliasing,
which the renderer does not emulate, the weights of a line add up). So `0 0 21 22 21 0 0` is 0 / 64 / 0, an unfiltered copy,
and `8 8 10 12 10 8 8` is 16 / 32 / 16: the 1-2-1 vertical blur that stops interlaced video from flickering. Taps that sum to
less than 64 darken the copy (games fade the screen out that way), more than 64 brighten it (clamped to 255). The result is
`(above * U + centre * C + below * D + 32) / 64` per RGBA channel; depth copies are not filtered. All taps zero (a program that
never called `GXSetCopyFilter`) counts as unfiltered, not as black.

The lines above the first and below the last of the copy rectangle are read from the EFB, unless the clamp bit of that side
(command bits 0 and 1) says to repeat the edge line, or the rectangle touches the edge of the EFB. Tales of Symphonia (PAL)
sets deflicker taps and both clamp bits for every copy, texture copies included, so its frames are softened by 1-2-1 in
the vertical direction, as on a console; `GP_COPY_FILTER=off` presents and copies them unfiltered.

Implementation: texture copies read the lines of the rectangle plus the margins back and filter them on the CPU before encoding
(`apply_copy_filter`, `tests/efb_copy_test.cpp`); a presented frame is drawn into the window by a small pass that samples the
EFB three times, one line apart, and weighs the samples (`present_filtered`, `tests/gp_render_check.cpp`; with an unfiltered copy the
plain `glBlitFramebuffer` is used as before). The window stretch is the same bilinear one in both paths; at the edge of the
rectangle the repeated line is approximated by clamping the sample position.
