#pragma once

#include "Ktx2.hpp"

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Where a source image comes from: a plain image file, or an image inside a .gltf/.glb.
 */
struct ImageSource
{
	// An image file, or the .gltf/.glb that contains the image.
	std::string Path;

	// >= 0: index into the glTF's images (Path is then a .gltf/.glb). -1: Path is an image file.
	int GltfImageIndex = -1;

	// Which channel to read when a single component is wanted (0 = R, 1 = G, 2 = B, 3 = A).
	// -1 reads the whole image (RGBA), or its luminance when a single component is requested.
	int Channel = -1;

	bool Empty() const { return Path.empty(); }

	bool operator==(const ImageSource& other) const
	{
		return Path == other.Path && GltfImageIndex == other.GltfImageIndex && Channel == other.Channel;
	}
};

/**
 * @brief One texture slot of a glTF material.
 */
struct GltfTextureSlot
{
	ImageSource Source;
	int Width = 0;
	int Height = 0;

	bool Empty() const { return Source.Empty(); }
};

struct GltfMaterial
{
	std::string Name;

	GltfTextureSlot BaseColor;
	GltfTextureSlot Normal;
	GltfTextureSlot Occlusion;
	GltfTextureSlot Roughness; // green channel of the metallic-roughness texture
	GltfTextureSlot Metallic;  // blue channel of the metallic-roughness texture
};

bool IsGltfPath(const std::string& path);

// Lists the materials of a .gltf/.glb along with the images their textures use.
// Returns false and sets `error` if the file cannot be read.
bool ListGltfMaterials(const std::string& path, std::vector<GltfMaterial>& materials, std::string& error);

// Loads the whole image as RGBA8.
bool LoadImageRGBA(const ImageSource& source, MPImage& image, std::string& error);

// Loads a single 8-bit component: the source's channel, or its luminance when Channel is -1.
bool LoadImageComponent(const ImageSource& source, int& width, int& height, std::vector<uint8_t>& pixels,
						std::string& error);

// Reads only the image header. Returns false if the image cannot be read.
bool GetImageSize(const ImageSource& source, int& width, int& height);
