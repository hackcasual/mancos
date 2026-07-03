#include "yafc/serialization/icon_scale.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include <miniz.h>

// stb_image: PNG decode only (icons are always PNG).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

// stb_image_write with miniz doing the deflate: stbiw's built-in compressor
// is fast but weak; mz_compress2 at MZ_BEST_COMPRESSION is ~zlib -9.
static unsigned char* YafcStbiwCompress(unsigned char* data, int dataLen,
                                        int* outLen, int /*quality*/) {
  mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(dataLen));
  auto* out = static_cast<unsigned char*>(std::malloc(bound));
  if (out == nullptr) return nullptr;
  mz_ulong written = bound;
  if (mz_compress2(out, &written, data, static_cast<mz_ulong>(dataLen),
                   MZ_BEST_COMPRESSION) != MZ_OK) {
    std::free(out);
    return nullptr;
  }
  *outLen = static_cast<int>(written);
  return out;
}
#define STBIW_ZLIB_COMPRESS YafcStbiwCompress
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace yafc {

std::string DownscaleIconPng(const std::string& pngBytes, int maxSide) {
  int w = 0, h = 0, comp = 0;
  unsigned char* pixels = stbi_load_from_memory(
      reinterpret_cast<const unsigned char*>(pngBytes.data()),
      static_cast<int>(pngBytes.size()), &w, &h, &comp, 4);
  if (pixels == nullptr || w <= 0 || h <= 0) {
    if (pixels != nullptr) stbi_image_free(pixels);
    return pngBytes;
  }

  // Base image = leading min(w,h) square (mip levels are appended after it).
  int side = w < h ? w : h;
  int target = side > maxSide ? maxSide : side;

  // Area-average downsample with alpha-weighted color (avoids dark fringes
  // from transparent texels). Exact for the common 64 -> 32 case.
  std::vector<unsigned char> scaled(static_cast<size_t>(target) * target * 4);
  double ratio = static_cast<double>(side) / target;
  for (int y = 0; y < target; ++y) {
    int y0 = static_cast<int>(y * ratio);
    int y1 = static_cast<int>((y + 1) * ratio);
    if (y1 <= y0) y1 = y0 + 1;
    if (y1 > side) y1 = side;
    for (int x = 0; x < target; ++x) {
      int x0 = static_cast<int>(x * ratio);
      int x1 = static_cast<int>((x + 1) * ratio);
      if (x1 <= x0) x1 = x0 + 1;
      if (x1 > side) x1 = side;
      double r = 0, g = 0, b = 0, a = 0;
      for (int sy = y0; sy < y1; ++sy) {
        const unsigned char* rowPx = pixels + (static_cast<size_t>(sy) * w + x0) * 4;
        for (int sx = x0; sx < x1; ++sx, rowPx += 4) {
          double alpha = rowPx[3];
          r += rowPx[0] * alpha;
          g += rowPx[1] * alpha;
          b += rowPx[2] * alpha;
          a += alpha;
        }
      }
      double count = static_cast<double>(x1 - x0) * (y1 - y0);
      unsigned char* out = scaled.data() + (static_cast<size_t>(y) * target + x) * 4;
      if (a > 0) {
        out[0] = static_cast<unsigned char>(r / a + 0.5);
        out[1] = static_cast<unsigned char>(g / a + 0.5);
        out[2] = static_cast<unsigned char>(b / a + 0.5);
      } else {
        out[0] = out[1] = out[2] = 0;
      }
      out[3] = static_cast<unsigned char>(a / count + 0.5);
    }
  }
  stbi_image_free(pixels);

  int pngLen = 0;
  unsigned char* png = stbi_write_png_to_mem(scaled.data(), target * 4, target,
                                             target, 4, &pngLen);
  if (png == nullptr) return pngBytes;
  std::string result(reinterpret_cast<char*>(png), static_cast<size_t>(pngLen));
  STBIW_FREE(png);

  // Unchanged dimensions and no size win: keep the original bytes.
  bool resized = target != w || target != h;
  if (!resized && result.size() >= pngBytes.size()) return pngBytes;
  return result;
}

}  // namespace yafc
