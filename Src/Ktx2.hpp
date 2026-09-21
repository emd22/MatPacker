#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MPImage
{
	int Width = 0;
	int Height = 0;
	std::vector<uint8_t> Pixels; // width * height * 4

	bool IsEmpty() const { return Pixels.empty(); }
};

// How the texture data is stored in the file.
enum class eTextureCompression
{
	None,		// raw RGBA8
	Zstd,		// raw RGBA8, Zstd-compressed on disk (lossless; same size in GPU memory)
	BasisUastc, // UASTC + Zstd; transcoded to BC7 / ASTC when loaded (lossy)
	Astc4x4		// ASTC 4x4 LDR, ready for the GPU as-is (lossy; mobile / Apple GPUs)
};

enum class eColorSpace
{
	Linear,
	SRGB
};

// Resizes to `width` x `height` (averaging when downscaling). sRGB images are filtered in linear light;
// `normal_map` renormalizes the vectors afterwards.
MPImage ResizeImage(const MPImage& src, int width, int height, eColorSpace space, bool normal_map);

// Writes an uncompressed RGBA8 KTX2 file. When `mipmaps` is set, a full mip chain is generated.
// Returns an empty string on success, otherwise an error message.
std::string WriteKTX2(const std::string& path, const MPImage& base, eColorSpace space, bool mipmaps, bool normal_map,
					  eTextureCompression compression = eTextureCompression::None);

// Reads every mip level (largest first) as RGBA8. Zstd and Basis files are decoded/transcoded; GPU-native
// formats such as ASTC cannot be shown and return an error.
// Returns an empty string on success, otherwise an error message.
std::string ReadKTX2Levels(const std::string& path, std::vector<MPImage>& levels);
