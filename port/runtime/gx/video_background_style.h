#pragma once
#include <algorithm>
#include <cstdint>

namespace gx::video_bg::style {

// The supplied Sky Stage Select Screen reference keeps the moving/art background around the
// perimeter and places a dark slate panel behind the stage grid. Return that panel's opacity for a
// decoded video pixel, with a short feather so arbitrary footage does not end at a hard rectangle.
inline uint8_t sss_matte_alpha(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
  if (!width || !height) return 0;
  const uint32_t left = width * 11 / 100, right = width * 89 / 100;
  const uint32_t top = height * 10 / 100, bottom = height * 92 / 100;
  if (x < left || x >= right || y < top || y >= bottom) return 0;
  const uint32_t feather_x = std::max(1u, width * 3 / 100);
  const uint32_t feather_y = std::max(1u, height * 4 / 100);
  const uint32_t dx = std::min(x - left, right - 1 - x);
  const uint32_t dy = std::min(y - top, bottom - 1 - y);
  const uint32_t ax = std::min(160u, 160u * (dx + 1) / feather_x);
  const uint32_t ay = std::min(160u, 160u * (dy + 1) / feather_y);
  return (uint8_t)std::min(ax, ay);
}

}  // namespace gx::video_bg::style
