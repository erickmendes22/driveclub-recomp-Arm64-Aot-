// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from shadPS4 e94b9798bc52e2454aac07a99dec39fb248a52ea,
// src/core/libraries/libpng/pngenc.cpp, using the existing libpng dependency.
// Each encode owns its libpng context, so overflow/failure does not poison the
// caller's reusable handle. No guest callback is passed to the host codec.

#include "png_enc.h"
#include "png_enc_error.h"
#include <climits>
#include <cstddef>
#include <cstring>
#include <png.h>

namespace Libraries::PngEnc {
namespace {
constexpr u64 Magic = 0x4443504e47454e43;
struct Handle {
    u64 magic;
    u32 max_width;
    u32 max_filters;
};
static_assert(sizeof(Handle) == 16);

bool ReadHandle(void* address, Handle& handle) {
    if (!address) return false;
    std::memcpy(&handle, address, sizeof(handle));
    return handle.magic == Magic && handle.max_width && handle.max_width <= 1000000 &&
           handle.max_filters <= 5;
}

int FilterMask(u16 filter) {
    if (!filter) return PNG_FILTER_NONE;
    if (filter == 15) return PNG_ALL_FILTERS;
    return ((filter & 1) ? PNG_FILTER_SUB : 0) | ((filter & 2) ? PNG_FILTER_UP : 0) |
           ((filter & 4) ? PNG_FILTER_AVG : 0) | ((filter & 8) ? PNG_FILTER_PAETH : 0);
}

struct Writer {
    u8* output;
    size_t capacity;
    size_t written;
    u32 rows;
    bool overflow;
};

void Write(png_structp png, png_bytep data, png_size_t length) {
    auto* writer = static_cast<volatile Writer*>(png_get_io_ptr(png));
    if (length > writer->capacity - writer->written || length > INT_MAX - writer->written) {
        writer->overflow = true;
        png_error(png, "PNG output buffer is too small");
    }
    std::memcpy(writer->output + writer->written, data, length);
    writer->written = writer->written + length;
}
void Flush(png_structp) {}
void Error(png_structp png, png_const_charp) { png_longjmp(png, 1); }
void Warning(png_structp, png_const_charp) {}
} // namespace

s32 PS4_SYSV_ABI scePngEncQueryMemorySize(const OrbisPngEncCreateParam* param) {
    if (!param) return ORBIS_PNG_ENC_ERROR_INVALID_ADDR;
    if (param->this_size != sizeof(*param)) return ORBIS_PNG_ENC_ERROR_INVALID_SIZE;
    if (param->attribute || param->max_filter_number > 5) return ORBIS_PNG_ENC_ERROR_INVALID_PARAM;
    if (!param->max_image_width || param->max_image_width > 1000000)
        return ORBIS_PNG_ENC_ERROR_INVALID_SIZE;
    return sizeof(Handle);
}

s32 PS4_SYSV_ABI scePngEncCreate(const OrbisPngEncCreateParam* param, void* memory,
                               u32 size, OrbisPngEncHandle* output) {
    if (!memory || !output) return ORBIS_PNG_ENC_ERROR_INVALID_ADDR;
    const s32 required = scePngEncQueryMemorySize(param);
    if (required < 0) return required;
    if (size < static_cast<u32>(required)) return ORBIS_PNG_ENC_ERROR_INVALID_SIZE;
    const Handle handle{Magic, param->max_image_width, param->max_filter_number};
    std::memcpy(memory, &handle, sizeof(handle));
    *output = memory;
    return 0;
}

s32 PS4_SYSV_ABI scePngEncDelete(OrbisPngEncHandle address) {
    Handle handle{};
    if (!ReadHandle(address, handle)) return ORBIS_PNG_ENC_ERROR_INVALID_HANDLE;
    std::memset(address, 0, sizeof(handle));
    return 0;
}

s32 PS4_SYSV_ABI scePngEncEncode(OrbisPngEncHandle address, const OrbisPngEncEncodeParam* param,
                               OrbisPngEncOutputInfo* output) {
    Handle handle{};
    if (!ReadHandle(address, handle)) return ORBIS_PNG_ENC_ERROR_INVALID_HANDLE;
    if (!param) return ORBIS_PNG_ENC_ERROR_INVALID_PARAM;
    if (!param->image_mem_addr || !param->png_mem_addr) return ORBIS_PNG_ENC_ERROR_INVALID_ADDR;
    if (!param->image_mem_size || !param->png_mem_size || !param->image_width || !param->image_height ||
        param->image_width > handle.max_width || param->image_height > PNG_UINT_31_MAX)
        return ORBIS_PNG_ENC_ERROR_INVALID_SIZE;
    if ((param->pixel_format != OrbisPngEncPixelFormat::R8G8B8A8 &&
         param->pixel_format != OrbisPngEncPixelFormat::B8G8R8A8) ||
        (param->color_space != OrbisPngEncColorSpace::RGB && param->color_space != OrbisPngEncColorSpace::RGBA) ||
        param->bit_depth != 8 || param->clut_number || param->filter_type > 15 || param->compression_level > 9)
        return ORBIS_PNG_ENC_ERROR_INVALID_PARAM;
    const u64 row_bytes = u64(param->image_width) * 4;
    const u64 pitch = param->image_pitch ? param->image_pitch : row_bytes;
    if (pitch < row_bytes || (pitch & 3)) return ORBIS_PNG_ENC_ERROR_INVALID_PARAM;
    if (u64(param->image_height - 1) * pitch + row_bytes > param->image_mem_size)
        return ORBIS_PNG_ENC_ERROR_INVALID_SIZE;
    if (output) *output = {};

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, Error, Warning);
    if (!png) return ORBIS_PNG_ENC_ERROR_FATAL;
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, nullptr);
        return ORBIS_PNG_ENC_ERROR_FATAL;
    }
    // libpng errors longjmp. Keep mutable state volatile and do not construct
    // C++ objects with destructors between this setjmp and the libpng calls.
    volatile Writer writer{param->png_mem_addr, param->png_mem_size, 0, 0, false};
    if (setjmp(png_jmpbuf(png))) {
        const bool overflow = writer.overflow;
        if (output) output->processed_height = writer.rows;
        png_destroy_write_struct(&png, &info);
        return overflow ? ORBIS_PNG_ENC_ERROR_DATA_OVERFLOW : ORBIS_PNG_ENC_ERROR_FATAL;
    }
    png_set_write_fn(png, const_cast<Writer*>(&writer), Write, Flush);
    const bool rgba = param->color_space == OrbisPngEncColorSpace::RGBA;
    png_set_IHDR(png, info, param->image_width, param->image_height, 8,
                 rgba ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_compression_level(png, param->compression_level);
    png_set_filter(png, PNG_FILTER_TYPE_BASE, FilterMask(param->filter_type));
    png_write_info(png, info);
    if (param->pixel_format == OrbisPngEncPixelFormat::B8G8R8A8) png_set_bgr(png);
    // On writes png_set_filler removes the extra alpha/filler byte from each
    // four-byte input pixel when the requested PNG has three color channels.
    if (!rgba) png_set_filler(png, 0, PNG_FILLER_AFTER);
    for (u32 y = 0; y < param->image_height; ++y) {
        png_write_row(png, const_cast<png_bytep>(param->image_mem_addr + size_t(y) * pitch));
        writer.rows = y + 1;
    }
    png_write_end(png, info);
    const auto written = static_cast<u32>(writer.written);
    png_destroy_write_struct(&png, &info);
    if (output) *output = {written, param->image_height};
    return static_cast<s32>(written);
}
} // namespace Libraries::PngEnc
