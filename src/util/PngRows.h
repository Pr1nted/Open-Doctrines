#pragma once

// A PNG decoded one row at a time, so the whole image never exists at once.
//
// WHY
//
// A map layer is 8192x4096. Decoded the ordinary way it is 128 MB of RGBA, and
// stb_image needs about as much again for the inflated scanlines while it
// works -- 256 MB in flight for an image whose every question is answered by
// two bytes a pixel (the province index) or one BIT a pixel (land or sea). On
// a 512 MB container host that transient is what decides whether the server
// starts at all. Streamed, the decoder holds the compressed data, a 32 KB
// inflate window and two scanlines.
//
// Each row is handed over as RGBA8 converted exactly as stb_image converts it
// when asked for four channels -- grey spread to RGB, palettes looked up,
// 16-bit samples cut to their high byte, missing alpha 255 -- so anything
// computed from the rows matches what the full decode gave.
// tests/png_rows_test.cpp checks that against stb_image on every shipped map.
//
// Not supported, and refused rather than guessed at: interlaced images. The
// caller falls back to the full decode for those.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

/**
 * Decode `data`, calling `row(y, rgba)` once per row, top to bottom, with
 * `width` * 4 bytes. `onSize(w, h)` is called first, before any row, and may
 * return false to stop. False with `why` set when the data is not a PNG this
 * can read; rows already delivered stay delivered.
 */
bool pngForEachRow(const uint8_t* data, size_t size,
                   const std::function<bool(int width, int height)>& onSize,
                   const std::function<void(int y, const uint8_t* rgba)>& row,
                   std::string* why = nullptr);
