#pragma once
#include <cstdint>
#include <vector>

/**
 * Indexed-PNG encoder for the map layers.
 *
 * WHY THIS EXISTS. A .odmap ships its layers as 8192x4096 PNGs, and stb's
 * writer only emits truecolour -- four bytes per pixel before deflate, 128 MB
 * of input for a layer that holds three distinct colours. land_sea.png is the
 * extreme case: the whole file answers one question per pixel (land or sea),
 * and LandSeaMap thresholds it back down to that bit the moment it loads.
 * Written as a 2-bit indexed PNG it is 44% of the truecolour size, decodes to
 * byte-identical RGBA, and needs no change on the reading side -- stb_image
 * expands PLTE/tRNS for us, so the game, the editor and every tool keep
 * calling LoadImageFromMemory exactly as before.
 *
 * Deliberately NOT a general PNG writer. It handles the one case that pays --
 * an image with few enough distinct colours to index -- and returns empty for
 * anything else so the caller falls back to stb rather than silently shipping
 * a worse encoding. provinces.png (1248 province ids as RGB) is such a case.
 */
namespace pngw {

/**
 * RGBA pixels -> indexed PNG at the smallest bit depth that fits (1/2/4/8).
 *
 * Empty when the image holds more than 256 distinct RGBA values, which is the
 * signal to encode it some other way. The palette is sorted, so the same
 * pixels always produce the same bytes -- shipped maps stay diffable and the
 * Python packer in tools/ can produce an identical file.
 */
std::vector<uint8_t> encodeIndexedRGBA(const uint8_t* rgba, int w, int h);

}  // namespace pngw
