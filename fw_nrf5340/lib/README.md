# Third-party encoders (unmodified copies)

| Directory | Source | Commit | License |
|---|---|---|---|
| `jpegenc/` | [bitbank2/JPEGENC](https://github.com/bitbank2/JPEGENC) `src/JPEGENC.h`, `src/jpegenc.inl` | `f51421bdaba8e098dc82130a17adaae6d61c3377` | Apache-2.0 (`jpegenc/LICENSE`) |
| `pngenc/` | [bitbank2/PNGenc](https://github.com/bitbank2/PNGenc) `src/PNGenc.h`, `src/png.inl` and its zlib subset | `23e97d1c146ef572fb9e073bbcafd2fbf212a57a` | Apache-2.0 (`pngenc/LICENSE`); zlib files under the zlib license (headers) |

**Patch** (`jpegenc/jpegenc.inl`, lines marked `VUEBUDS PATCH`): JPEGENC only has four
quality levels (the standard tables / 4, / 2, x 1, x 2). `JPEGEQuant()` keeps those for
`ucQFactor` 0–3 and adds `JPEGE_Q_IJG + q` (q = 1..100), which scales the tables like libjpeg's
`jpeg_set_quality()` (scale = 5000 / q below 50, else 200 − 2q), so q95 / 90 / 85 / 75 / 60 match
what OpenCV / libjpeg call those qualities. Nothing else is changed.

Both are written as C code inside `.inl` files wrapped by Arduino C++ classes. The C++
wrappers (`*.cpp`) are not copied; `src/codec.c` includes the `.inl` files as C.
