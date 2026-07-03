// Icon post-processing for bundles: crop mip strips to the base image,
// downscale to a size budget, and re-encode as PNG. Icons in mods are 64px+
// with appended mip levels; the web UI never renders above ~24 CSS px, so a
// 32px budget cuts bundle size sharply with no visible loss.
#pragma once

#include <string>

namespace yafc {

// Returns re-encoded PNG bytes: the leading min(w,h) square (drops appended
// mip levels), area-downsampled to at most maxSide pixels. Falls back to the
// input bytes unchanged when decoding fails or re-encoding would be larger
// with no dimension change.
std::string DownscaleIconPng(const std::string& pngBytes, int maxSide);

}  // namespace yafc
