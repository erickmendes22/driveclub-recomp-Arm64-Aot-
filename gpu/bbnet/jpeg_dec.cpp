// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// ABI and workspace sizing: shadPS4 e94b9798bc52e2454aac07a99dec39fb248a52ea,
// src/core/libraries/jpeg/jpegdec.cpp. The decoder is the same stb JPEG codec,
// with checked header segments, output bounds and explicit unsupported modes.

#include "jpeg_dec.h"
#include "jpeg_error.h"
#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstring>
#include <limits>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "../third_party/stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace Libraries::JpegDec {
namespace {
constexpr s32 Ok = 0;

bool ValidRatio(OrbisJpegDecDownscaleRatio ratio) {
    return ratio == OrbisJpegDecDownscaleRatio::Ratio1 ||
           ratio == OrbisJpegDecDownscaleRatio::Ratio2 ||
           ratio == OrbisJpegDecDownscaleRatio::Ratio4 ||
           ratio == OrbisJpegDecDownscaleRatio::Ratio8;
}

bool ValidHandle(OrbisJpegDecHandle handle) {
    return handle && !(reinterpret_cast<uintptr_t>(handle) & 31) &&
           handle->handle == handle && handle->handle_size == sizeof(OrbisJpegDecCreateParam) &&
           handle->decode_mode >= 1 && handle->decode_mode <= 2 &&
           handle->max_width && handle->max_width <= 65536;
}

bool HasCompleteStream(const void* data, size_t size) {
    const auto* bytes = static_cast<const u8*>(data);
    size_t position = 2; // SOI was checked by Parse.
    bool entropy = false, saw_scan = false;
    while (position < size) {
        if (bytes[position++] != 0xff) {
            if (!entropy) return false;
            continue;
        }
        while (position < size && bytes[position] == 0xff) ++position;
        if (position == size) return false;
        const u8 marker = bytes[position++];
        if (!marker || (marker >= 0xd0 && marker <= 0xd7)) {
            if (!entropy) return false;
            continue;
        }
        if (marker == 0xd9) return saw_scan;
        if (marker == 0xd8) return false;
        if (marker == 0x01) continue;
        if (size - position < 2) return false;
        const size_t length = (size_t(bytes[position]) << 8) | bytes[position + 1];
        if (length < 2 || length > size - position) return false;
        entropy = marker == 0xda;
        saw_scan |= entropy;
        position += length;
    }
    return false;
}

s32 Parse(const OrbisJpegDecParseParam* param, OrbisJpegDecImageInfo* output) {
    if (!param || !param->jpeg_addr || !output) return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    if (!param->jpeg_size || param->jpeg_size > INT_MAX) return ORBIS_JPEG_DEC_ERROR_INVALID_SIZE;
    if (!ValidRatio(param->downscale_ratio) || static_cast<u16>(param->decode_mode) > 1)
        return ORBIS_JPEG_DEC_ERROR_INVALID_PARAM;
    // MJPEG input control / abbreviated streams are not implemented by this provider.
    if (param->decode_mode != OrbisJpegDecDecodeMode::Normal)
        return ORBIS_JPEG_DEC_ERROR_UNSUPPORT_DATA;
    const auto* bytes = static_cast<const u8*>(param->jpeg_addr);
    const size_t size = param->jpeg_size;
    if (size < 4 || bytes[0] != 0xff || bytes[1] != 0xd8)
        return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
    size_t position = 2;
    while (position < size) {
        if (bytes[position++] != 0xff) return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
        while (position < size && bytes[position] == 0xff) ++position;
        if (position == size) return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
        const u8 marker = bytes[position++];
        if (marker == 0xda || marker == 0xd9) break;
        if (!marker || marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7))
            return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
        if (marker == 0x01) continue; // TEM has no length field.
        if (size - position < 2) return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
        const size_t length = (size_t(bytes[position]) << 8) | bytes[position + 1];
        if (length < 2 || length > size - position) return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
        if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2) {
            if (length < 8) return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
            const u8* frame = bytes + position;
            const u16 components = frame[7];
            if (length != 8 + size_t(components) * 3) return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
            if (frame[2] != 8 || (components != 1 && components != 3))
                return ORBIS_JPEG_DEC_ERROR_UNSUPPORT_DATA;
            OrbisJpegDecImageInfo info{};
            info.image_height = (u32(frame[3]) << 8) | frame[4];
            info.image_width = (u32(frame[5]) << 8) | frame[6];
            if (!info.image_width || !info.image_height) return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
            info.num_components = components;
            info.color_space = components == 1 ? OrbisJpegDecColorSpace::Grayscale
                                               : OrbisJpegDecColorSpace::Ycc;
            u32 max_h = 1, density = 0;
            for (u16 c = 0; c < components; ++c) {
                const u8* component = frame + 8 + c * 3;
                const u32 h = component[1] >> 4, v = component[1] & 15;
                if (!h || h > 4 || !v || v > 4 || component[2] > 3)
                    return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
                for (u16 previous = 0; previous < c; ++previous)
                    if (info.component_id[previous] == component[0])
                        return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
                info.component_id[c] = component[0];
                info.sampling_factor[c] = component[1];
                max_h = std::max(max_h, h);
                density += h * v;
            }
            // Offset 0x14 is an optional auxiliary allocation size in the actual
            // Driveclub caller. stb owns its temporary decode memory; no guest
            // auxiliary buffer is required. Do not report the parse mode here.
            info.auxiliary_bytes = 0;
            info.required_decode_mode = components == 1 || density * (96 / (max_h * 8)) < 0x25 ? 1 : 2;
            const u32 ratio = static_cast<u16>(param->downscale_ratio);
            info.output_width = (info.image_width + ratio - 1) / ratio;
            info.output_height = (info.image_height + ratio - 1) / ratio;
            *output = info;
            return Ok;
        }
        // Other SOF variants (lossless/arithmetic/12-bit) are not stb JPEG inputs.
        if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc)
            return ORBIS_JPEG_DEC_ERROR_UNSUPPORT_DATA;
        position += length;
    }
    return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
}
} // namespace

s32 PS4_SYSV_ABI sceJpegDecParseHeader(const OrbisJpegDecParseParam* param,
                                     OrbisJpegDecImageInfo* info) {
    return Parse(param, info);
}

s32 PS4_SYSV_ABI sceJpegDecQueryMemorySize(const OrbisJpegDecCreateParam* param) {
    if (!param) return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    if (param->size != sizeof(*param)) return ORBIS_JPEG_DEC_ERROR_INVALID_SIZE;
    if (param->decode_mode < 1 || param->decode_mode > 2 || !param->max_width || param->max_width > 65535)
        return ORBIS_JPEG_DEC_ERROR_INVALID_PARAM;
    const u32 width = ((param->max_width + 31) & ~31u) | 16;
    return static_cast<s32>(width * (param->decode_mode == 2 ? 160 : 48) + width * 6 + 0x7360);
}

s32 PS4_SYSV_ABI sceJpegDecCreate(const OrbisJpegDecCreateParam* param, void* memory,
                                u32 size, OrbisJpegDecHandle* output) {
    if (!param || !memory || !output) return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    const s32 required = sceJpegDecQueryMemorySize(param);
    if (required < 0) return required;
    if (size < static_cast<u32>(required)) return ORBIS_JPEG_DEC_ERROR_INVALID_SIZE;
    const auto address = reinterpret_cast<uintptr_t>(memory);
    if (address > std::numeric_limits<uintptr_t>::max() - 31)
        return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    auto* handle = reinterpret_cast<OrbisJpegDecHandle>((address + 31) & ~uintptr_t(31));
    *handle = {handle, sizeof(*param), param->decode_mode, (param->max_width + 31) & ~31u, 0};
    *output = handle;
    return Ok;
}

s32 PS4_SYSV_ABI sceJpegDecDelete(OrbisJpegDecHandle handle) {
    if (!ValidHandle(handle)) return ORBIS_JPEG_DEC_ERROR_INVALID_HANDLE;
    handle->handle = nullptr;
    return Ok;
}

s32 PS4_SYSV_ABI sceJpegDecDecode(OrbisJpegDecHandle handle, const OrbisJpegDecDecodeParam* param,
                                OrbisJpegDecImageInfo* output) {
    if (!ValidHandle(handle)) return ORBIS_JPEG_DEC_ERROR_INVALID_HANDLE;
    if (!param || !param->image_addr) return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    if (!param->image_size) return ORBIS_JPEG_DEC_ERROR_INVALID_SIZE;
    const auto format = param->pixel_format;
    if (format == OrbisJpegDecPixelFormat::Y8U8Y8V8) return ORBIS_JPEG_DEC_ERROR_UNSUPPORT_DATA;
    if (format != OrbisJpegDecPixelFormat::R8G8B8A8 && format != OrbisJpegDecPixelFormat::B8G8R8A8 &&
        format != OrbisJpegDecPixelFormat::Y8) return ORBIS_JPEG_DEC_ERROR_INVALID_PARAM;
    if (param->alpha_value > 255) return ORBIS_JPEG_DEC_ERROR_INVALID_PARAM;
    const OrbisJpegDecParseParam parse{param->jpeg_addr, param->jpeg_size,
                                     param->decode_mode, param->downscale_ratio};
    OrbisJpegDecImageInfo info{};
    const s32 parsed = Parse(&parse, &info);
    if (parsed < 0) return parsed;
    if (info.image_width > handle->max_width) return ORBIS_JPEG_DEC_ERROR_EXCEED_MAX_WIDTH;
    const u32 channels = format == OrbisJpegDecPixelFormat::Y8 ? 1 : 4;
    const u32 row_bytes = info.output_width * channels;
    const u32 pitch = param->image_pitch ? param->image_pitch : row_bytes;
    if (pitch < row_bytes || (channels == 4 && (pitch & 3))) return ORBIS_JPEG_DEC_ERROR_INVALID_PARAM;
    if (u64(info.output_height - 1) * pitch + row_bytes > param->image_size)
        return ORBIS_JPEG_DEC_ERROR_INVALID_SIZE;
    if (!HasCompleteStream(param->jpeg_addr, param->jpeg_size)) return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
    int width = 0, height = 0, components = 0;
    stbi_uc* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(param->jpeg_addr),
                                           static_cast<int>(param->jpeg_size), &width, &height,
                                           &components, static_cast<int>(channels));
    if (!pixels) return ORBIS_JPEG_DEC_ERROR_DECODE_ERROR;
    if (u32(width) != info.image_width || u32(height) != info.image_height) {
        stbi_image_free(pixels);
        return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
    }
    const u32 ratio = static_cast<u16>(param->downscale_ratio);
    auto* destination = static_cast<u8*>(param->image_addr);
    for (u32 y = 0; y < info.output_height; ++y) {
        u8* row = destination + size_t(y) * pitch;
        for (u32 x = 0; x < info.output_width; ++x) {
            const u8* source = pixels + (size_t(y * ratio) * width + x * ratio) * channels;
            if (channels == 1) {
                row[x] = source[0];
            } else {
                const bool bgra = format == OrbisJpegDecPixelFormat::B8G8R8A8;
                row[x * 4] = source[bgra ? 2 : 0];
                row[x * 4 + 1] = source[1];
                row[x * 4 + 2] = source[bgra ? 0 : 2];
                row[x * 4 + 3] = static_cast<u8>(param->alpha_value);
            }
        }
    }
    stbi_image_free(pixels);
    if (output) *output = info;
    return static_cast<s32>((info.output_width << 16) | info.output_height);
}
} // namespace Libraries::JpegDec
