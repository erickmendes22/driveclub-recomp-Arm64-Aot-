// Actual HLE codec implementations, with generated JPEG data and PNG output
// independently decoded by libpng. No emulator, game executable or GPU needed.
#include "../gpu/bbnet/jpeg_dec.h"
#include "../gpu/bbnet/jpeg_error.h"
#include "../gpu/bbnet/png_enc.h"
#include "../gpu/bbnet/png_enc_error.h"
#include "fixtures/image_codec_jpeg.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstring>
#include <vector>
#include <png.h>

static bool Near(unsigned value, unsigned expected) {
    return value + 2 >= expected && expected + 2 >= value;
}

static void TestJpeg() {
    using namespace Libraries::JpegDec;
    OrbisJpegDecImageInfo info{};
    OrbisJpegDecParseParam parse{ColorJpeg, sizeof(ColorJpeg), OrbisJpegDecDecodeMode::Normal,
                               OrbisJpegDecDownscaleRatio::Ratio1};
    assert(!sceJpegDecParseHeader(&parse, &info));
    assert(info.image_width == 8 && info.image_height == 5 && info.output_width == 8 && info.output_height == 5);
    assert(info.num_components == 3 && info.color_space == OrbisJpegDecColorSpace::Ycc);
    assert(info.sampling_factor[0] == 0x11 && !info.auxiliary_bytes);
    OrbisJpegDecCreateParam create{sizeof(create), info.required_decode_mode, info.output_width};
    const s32 required = sceJpegDecQueryMemorySize(&create);
    assert(required > 0);
    std::vector<u8> work(required + 64, 0x9d);
    OrbisJpegDecHandle handle = nullptr;
    assert(sceJpegDecCreate(&create, work.data() + 1, required - 1, &handle) == ORBIS_JPEG_DEC_ERROR_INVALID_SIZE);
    assert(!handle);
    assert(!sceJpegDecCreate(&create, work.data() + 1, required, &handle));
    assert(!(reinterpret_cast<uintptr_t>(handle) & 31));
    assert(std::all_of(work.begin() + required + 1, work.end(), [](u8 value) { return value == 0x9d; }));

    constexpr u32 pitch = 44;
    std::vector<u8> pixels(pitch * 5 + 32, 0xcc);
    OrbisJpegDecDecodeParam decode{ColorJpeg, pixels.data(), nullptr, sizeof(ColorJpeg), pitch * 5, 0,
        OrbisJpegDecDecodeMode::Normal, OrbisJpegDecDownscaleRatio::Ratio1,
        OrbisJpegDecPixelFormat::B8G8R8A8, 255, pitch};
    assert(sceJpegDecDecode(handle, &decode, &info) == (8 << 16 | 5));
    for (unsigned y = 0; y < 5; ++y) {
        for (unsigned x = 0; x < 8; ++x) {
            const u8* pixel = pixels.data() + y * pitch + x * 4;
            assert(Near(pixel[0], 99) && Near(pixel[1], 51) && Near(pixel[2], 201) && pixel[3] == 255);
        }
        for (unsigned x = 32; x < pitch; ++x) assert(pixels[y * pitch + x] == 0xcc);
    }
    assert(std::all_of(pixels.begin() + pitch * 5, pixels.end(), [](u8 value) { return value == 0xcc; }));
    // The actual game uses pitch=0, BGRA and alpha=255. Also verify explicit
    // transparent alpha is preserved rather than silently converted to opaque.
    decode.image_pitch = 0;
    decode.pixel_format = OrbisJpegDecPixelFormat::R8G8B8A8;
    decode.alpha_value = 0;
    assert(sceJpegDecDecode(handle, &decode, nullptr) == (8 << 16 | 5));
    assert(Near(pixels[0], 201) && Near(pixels[2], 99) && pixels[3] == 0);
    decode.jpeg_addr = ProgressiveJpeg;
    decode.jpeg_size = sizeof(ProgressiveJpeg);
    assert(sceJpegDecDecode(handle, &decode, &info) == (8 << 16 | 5));
    assert(info.sampling_factor[0] == 0x11 && Near(pixels[0], 201));

    decode.jpeg_addr = GrayJpeg;
    decode.jpeg_size = sizeof(GrayJpeg);
    decode.pixel_format = OrbisJpegDecPixelFormat::Y8;
    for (unsigned ratio : {1u, 2u, 4u, 8u}) {
        decode.downscale_ratio = static_cast<OrbisJpegDecDownscaleRatio>(ratio);
        const unsigned width = (7 + ratio - 1) / ratio, height = (5 + ratio - 1) / ratio;
        assert(sceJpegDecDecode(handle, &decode, &info) == static_cast<s32>((width << 16) | height));
        assert(info.num_components == 1 && info.color_space == OrbisJpegDecColorSpace::Grayscale);
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x)
                assert(Near(pixels[y * width + x], GrayExpected[y * ratio * 7 + x * ratio]));
    }

    decode = {ColorJpeg, pixels.data(), nullptr, sizeof(ColorJpeg), pitch * 5, 0,
        OrbisJpegDecDecodeMode::Normal, OrbisJpegDecDownscaleRatio::Ratio1,
        OrbisJpegDecPixelFormat::B8G8R8A8, 255, pitch};
    std::fill(pixels.begin(), pixels.end(), 0xd3);
    const auto saved = pixels;
    decode.image_size = 1;
    assert(sceJpegDecDecode(handle, &decode, nullptr) == ORBIS_JPEG_DEC_ERROR_INVALID_SIZE);
    decode.image_size = pitch * 5;
    decode.image_pitch = 31;
    assert(sceJpegDecDecode(handle, &decode, nullptr) == ORBIS_JPEG_DEC_ERROR_INVALID_PARAM);
    decode.image_pitch = pitch;
    decode.pixel_format = static_cast<OrbisJpegDecPixelFormat>(65535);
    assert(sceJpegDecDecode(handle, &decode, nullptr) == ORBIS_JPEG_DEC_ERROR_INVALID_PARAM);
    decode.pixel_format = OrbisJpegDecPixelFormat::Y8U8Y8V8;
    assert(sceJpegDecDecode(handle, &decode, nullptr) == ORBIS_JPEG_DEC_ERROR_UNSUPPORT_DATA);
    decode.pixel_format = OrbisJpegDecPixelFormat::B8G8R8A8;
    decode.jpeg_size = sizeof(ColorJpeg) - 2;
    assert(sceJpegDecDecode(handle, &decode, nullptr) == ORBIS_JPEG_DEC_ERROR_INVALID_DATA);
    decode.jpeg_size = UINT32_MAX;
    assert(sceJpegDecDecode(handle, &decode, nullptr) == ORBIS_JPEG_DEC_ERROR_INVALID_SIZE);
    assert(pixels == saved);
    parse.jpeg_addr = CmykJpeg;
    parse.jpeg_size = sizeof(CmykJpeg);
    assert(sceJpegDecParseHeader(&parse, &info) == ORBIS_JPEG_DEC_ERROR_UNSUPPORT_DATA);
    const u8 malformed[] = {0xff, 0xd8, 0xff, 0xc0, 0, 17, 8, 0, 5};
    parse.jpeg_addr = malformed;
    parse.jpeg_size = sizeof(malformed);
    assert(sceJpegDecParseHeader(&parse, &info) == ORBIS_JPEG_DEC_ERROR_INVALID_DATA);
    parse.jpeg_size = 0;
    assert(sceJpegDecParseHeader(&parse, &info) == ORBIS_JPEG_DEC_ERROR_INVALID_SIZE);
    assert(sceJpegDecParseHeader(nullptr, &info) == ORBIS_JPEG_DEC_ERROR_INVALID_ADDR);
    assert(!sceJpegDecDelete(handle));
    assert(sceJpegDecDelete(handle) == ORBIS_JPEG_DEC_ERROR_INVALID_HANDLE);
    assert(sceJpegDecDecode(handle, &decode, nullptr) == ORBIS_JPEG_DEC_ERROR_INVALID_HANDLE);
    std::puts("PASS: JPEG baseline/progressive/grayscale, BGRA/RGBA/Y8, scaling, pitch, alpha and rejected invalid input");
}

static void TestPng() {
    using namespace Libraries::PngEnc;
    OrbisPngEncCreateParam create{sizeof(create), 0, 3, 4};
    const s32 required = scePngEncQueryMemorySize(&create);
    assert(required > 0 && required < 64);
    std::array<u8, 64> work;
    work.fill(0x9d);
    OrbisPngEncHandle handle = nullptr;
    assert(scePngEncCreate(&create, work.data(), required - 1, &handle) == ORBIS_PNG_ENC_ERROR_INVALID_SIZE);
    assert(!handle);
    assert(!scePngEncCreate(&create, work.data(), required, &handle));
    assert(std::all_of(work.begin() + required, work.end(), [](u8 value) { return value == 0x9d; }));

    constexpr u32 width = 3, height = 2, pitch = 16;
    const std::array<u8, 24> rgba{
        201, 51, 99, 255, 20, 220, 40, 0, 50, 80, 240, 180,
        250, 240, 30, 70, 10, 20, 30, 255, 150, 100, 50, 128};
    std::array<u8, pitch * height> source;
    std::vector<u8> encoded(1024, 0xcc);
    OrbisPngEncOutputInfo output{};
    OrbisPngEncEncodeParam encode{source.data(), encoded.data(), source.size(), 1000, width, height, pitch,
        OrbisPngEncPixelFormat::R8G8B8A8, OrbisPngEncColorSpace::RGBA, 8, 0, 15, 6};
    for (bool bgra : {false, true}) {
        source.fill(0xee);
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x) {
                const u8* pixel = rgba.data() + (y * width + x) * 4;
                u8* target = source.data() + y * pitch + x * 4;
                target[0] = pixel[bgra ? 2 : 0]; target[1] = pixel[1];
                target[2] = pixel[bgra ? 0 : 2]; target[3] = pixel[3];
            }
        encode.pixel_format = bgra ? OrbisPngEncPixelFormat::B8G8R8A8 : OrbisPngEncPixelFormat::R8G8B8A8;
        for (bool alpha : {false, true}) {
            encode.color_space = alpha ? OrbisPngEncColorSpace::RGBA : OrbisPngEncColorSpace::RGB;
            const auto input_before = source;
            const s32 bytes = scePngEncEncode(handle, &encode, &output);
            assert(bytes > 0 && output.data_size == u32(bytes) && output.processed_height == height);
            assert(source == input_before);
            png_image image{}; image.version = PNG_IMAGE_VERSION;
            assert(png_image_begin_read_from_memory(&image, encoded.data(), bytes));
            assert(image.width == width && image.height == height);
            assert(bool(image.format & PNG_FORMAT_FLAG_ALPHA) == alpha);
            image.format = PNG_FORMAT_RGBA;
            std::array<u8, 24> decoded{};
            assert(png_image_finish_read(&image, nullptr, decoded.data(), 0, nullptr));
            png_image_free(&image);
            for (size_t i = 0; i < decoded.size(); ++i)
                assert(decoded[i] == ((!alpha && i % 4 == 3) ? 255 : rgba[i]));
        }
    }
    assert(std::all_of(encoded.begin() + encode.png_mem_size, encoded.end(), [](u8 value) { return value == 0xcc; }));
    // Exercise failure inside libpng's write callback, then reuse the same
    // handle successfully. No output write may cross the caller's capacity.
    std::fill(encoded.begin(), encoded.end(), 0xa7);
    encode.png_mem_size = 16;
    assert(scePngEncEncode(handle, &encode, &output) == ORBIS_PNG_ENC_ERROR_DATA_OVERFLOW);
    assert(!output.data_size);
    assert(std::all_of(encoded.begin() + 16, encoded.end(), [](u8 value) { return value == 0xa7; }));
    encode.png_mem_size = 1000;
    assert(scePngEncEncode(handle, &encode, nullptr) > 0);
    encode.image_mem_size = 27;
    assert(scePngEncEncode(handle, &encode, nullptr) == ORBIS_PNG_ENC_ERROR_INVALID_SIZE);
    encode.image_mem_size = source.size();
    encode.image_pitch = 8;
    assert(scePngEncEncode(handle, &encode, nullptr) == ORBIS_PNG_ENC_ERROR_INVALID_PARAM);
    encode.image_pitch = pitch;
    encode.image_height = UINT32_MAX;
    assert(scePngEncEncode(handle, &encode, nullptr) == ORBIS_PNG_ENC_ERROR_INVALID_SIZE);
    encode.image_height = height;
    encode.compression_level = 10;
    assert(scePngEncEncode(handle, &encode, nullptr) == ORBIS_PNG_ENC_ERROR_INVALID_PARAM);
    encode.compression_level = 6;
    encode.bit_depth = 16;
    assert(scePngEncEncode(handle, &encode, nullptr) == ORBIS_PNG_ENC_ERROR_INVALID_PARAM);
    encode.bit_depth = 8;
    encode.pixel_format = static_cast<OrbisPngEncPixelFormat>(65535);
    assert(scePngEncEncode(handle, &encode, nullptr) == ORBIS_PNG_ENC_ERROR_INVALID_PARAM);
    assert(scePngEncEncode(handle, nullptr, nullptr) == ORBIS_PNG_ENC_ERROR_INVALID_PARAM);
    assert(scePngEncQueryMemorySize(nullptr) == ORBIS_PNG_ENC_ERROR_INVALID_ADDR);
    assert(!scePngEncDelete(handle));
    assert(scePngEncDelete(handle) == ORBIS_PNG_ENC_ERROR_INVALID_HANDLE);
    assert(scePngEncEncode(handle, &encode, nullptr) == ORBIS_PNG_ENC_ERROR_INVALID_HANDLE);
    std::puts("PASS: PNG RGBA/BGRA to RGB/RGBA round trips, real pitched rows, bounded output and reusable handles after overflow");
}

int main() {
    TestJpeg();
    TestPng();
    std::puts("PASS: actual JPEG/PNG providers, ABI sizes and codec error contracts");
}
