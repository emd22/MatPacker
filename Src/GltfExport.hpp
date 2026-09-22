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

// Writes a copy of the .gltf/.glb at `source_path` to `output_path` with the listed materials pointing at their baked
// .ktx2 files (through KHR_texture_basisu). The output is binary if `output_path` ends in .glb, otherwise a .gltf
// (with a .bin beside it if the source had embedded binary data). Relative URIs are rewritten so they still resolve
// from the output's folder.
// With `embed_textures`, the .ktx2 files are stored in the output itself (a .glb's binary chunk, or base64 data URIs
// in a .gltf) instead of being referenced. Returns false and sets `error` on failure.
bool ExportGltfWithBakedMaterials(const std::string& source_path, const std::string& output_path,
								  const std::vector<BakedMaterialLink>& links, bool embed_textures,
								  const std::function<void(const std::string&)>& log, std::string& error);
