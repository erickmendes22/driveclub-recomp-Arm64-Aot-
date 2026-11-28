# stb JPEG decoder

`stb_image.h` is copied from the same primary source revision as the JPEG HLE ABI:

- Repository: https://github.com/shadps4-emu/shadPS4
- Commit: `e94b9798bc52e2454aac07a99dec39fb248a52ea`
- Path: `externals/stb/stb_image.h` (stb_image 2.30)
- Upstream Git blob: `9eedabedc45b3e6fd88fae6f14a160b4d53272ec`
- Local SHA-256: `b00dffc0175fbf1c6174a5c31e30be5721cb3f764f19429a8e9b0d1027920884`

The complete upstream dual public-domain/MIT license is included at the end of
the header. `bbnet/jpeg_dec.cpp` compiles only the JPEG, in-memory decoder with
translation-unit-local symbols; no desktop codec library is added to the APK.
