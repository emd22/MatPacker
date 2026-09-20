#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A tightly packed RGBA8 image.
struct Image8 {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // width * height * 4

    bool Empty() const { return pixels.empty(); }
};

enum class ColorSpace { Linear, SRGB };

// Resizes to `width` x `height` (averaging when downscaling). sRGB images are filtered in linear light;
// `normal_map` renormalizes the vectors afterwards.
Image8 ResizeImage(const Image8& src, int width, int height, ColorSpace space, bool normal_map);

// Writes an uncompressed RGBA8 KTX2 file. When `mipmaps` is set, a full mip chain is generated.
// Returns an empty string on success, otherwise an error message.
std::string WriteKtx2(const std::string& path, const Image8& base, ColorSpace space, bool mipmaps,
                      bool normal_map);

// Reads every mip level (largest first) of an uncompressed RGBA8 KTX2 file.
// Returns an empty string on success, otherwise an error message.
std::string ReadKtx2Levels(const std::string& path, std::vector<Image8>& levels);
