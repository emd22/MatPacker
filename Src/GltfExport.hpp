#pragma once

#include <functional>
#include <string>
#include <vector>

/**
 * @brief The baked textures to link into one glTF material. An empty path leaves that slot as it was.
 */
struct BakedMaterialLink
{
	int MaterialIndex = -1;

	std::string BaseColor;
	std::string Normal;
	std::string MetallicRoughness; // the ORM texture (G = roughness, B = metallic)
	std::string Occlusion;		   // the ORM texture (R = AO)
};

struct GltfExportOptions
{
	// Store the .ktx2 files in the output (a .glb's binary chunk, or base64 data URIs in a .gltf) instead of
	// referencing them.
	bool bEmbedTextures = false;

	// Name the .ktx2 images through KHR_texture_basisu, as the glTF spec requires. When off, textures name them in
	// the core `source`, which is not valid glTF but is read by tools that do not know the extension.
	bool bUseBasisUExtension = true;
};

// Writes a copy of the .gltf/.glb at `source_path` to `output_path` with the listed materials pointing at their baked
// .ktx2 files (through KHR_texture_basisu). The output is binary if `output_path` ends in .glb, otherwise a .gltf
// (with a .bin beside it if the source had embedded binary data). Relative URIs are rewritten so they still resolve
// from the output's folder, and textures, images and binary data no longer used are removed.
// Returns false and sets `error` on failure.
bool ExportGltfWithBakedMaterials(const std::string& source_path, const std::string& output_path,
								  const std::vector<BakedMaterialLink>& links, const GltfExportOptions& options,
								  const std::function<void(const std::string&)>& log, std::string& error);
