#include "ImageSource.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "cgltf.h"
#include "stb_image.h"

namespace fs = std::filesystem;

namespace {

// Owns a parsed glTF and frees it on scope exit.
struct GltfHandle
{
	cgltf_data* Data = nullptr;

	GltfHandle() = default;
	GltfHandle(const GltfHandle&) = delete;
	GltfHandle& operator=(const GltfHandle&) = delete;

	~GltfHandle()
	{
		if (Data) {
			cgltf_free(Data);
		}
	}
};

std::string GltfResultMessage(cgltf_result result)
{
	switch (result) {
	case cgltf_result_file_not_found:
		return "file not found";
	case cgltf_result_io_error:
		return "i/o error";
	case cgltf_result_invalid_json:
		return "invalid json";
	case cgltf_result_invalid_gltf:
		return "invalid glTF";
	case cgltf_result_out_of_memory:
		return "out of memory";
	case cgltf_result_unknown_format:
		return "unknown format";
	default:
		return "error " + std::to_string(int(result));
	}
}

bool ParseGltf(const std::string& path, GltfHandle& handle, std::string& error)
{
	cgltf_options options = {};
	cgltf_result result = cgltf_parse_file(&options, path.c_str(), &handle.Data);

	if (result != cgltf_result_success) {
		error = "failed to read glTF '" + path + "': " + GltfResultMessage(result);
		return false;
	}

	return true;
}

// An image's encoded bytes (png/jpg/...) held in memory, or the path of a file to read instead.
struct EncodedImage
{
	std::vector<uint8_t> Bytes;
	std::string FilePath;
};

bool HasPrefix(const char* text, const char* prefix) { return std::strncmp(text, prefix, std::strlen(prefix)) == 0; }

// Fetches the encoded bytes of image `index`. `data` must have its buffers loaded if the image is in a buffer view.
bool FetchGltfImage(const cgltf_data* data, int index, const std::string& gltf_path, EncodedImage& encoded,
					std::string& error)
{
	if (index < 0 || size_t(index) >= data->images_count) {
		error = "glTF image index " + std::to_string(index) + " is out of range";
		return false;
	}

	const cgltf_image& image = data->images[index];

	if (image.buffer_view) {
		const cgltf_buffer_view* view = image.buffer_view;

		if (!view->buffer->data) {
			error = "glTF image " + std::to_string(index) + " has no loaded buffer data";
			return false;
		}

		const uint8_t* begin = static_cast<const uint8_t*>(view->buffer->data) + view->offset;
		encoded.Bytes.assign(begin, begin + view->size);
		return true;
	}

	if (!image.uri) {
		error = "glTF image " + std::to_string(index) + " has no data";
		return false;
	}

	if (HasPrefix(image.uri, "data:")) {
		const char* comma = std::strchr(image.uri, ',');

		if (!comma) {
			error = "glTF image " + std::to_string(index) + " has a malformed data URI";
			return false;
		}

		const char* payload = comma + 1;
		const size_t length = std::strlen(payload);
		size_t padding = 0;

		while (padding < length && payload[length - 1 - padding] == '=') {
			padding++;
		}

		cgltf_options options = {};
		void* decoded = nullptr;
		const size_t decoded_size = length / 4 * 3 - padding;

		if (cgltf_load_buffer_base64(&options, decoded_size, payload, &decoded) != cgltf_result_success) {
			error = "failed to decode the embedded image " + std::to_string(index);
			return false;
		}

		const uint8_t* begin = static_cast<const uint8_t*>(decoded);
		encoded.Bytes.assign(begin, begin + decoded_size);
		free(decoded);
		return true;
	}

	// External file, relative to the glTF.
	std::string relative = image.uri;
	cgltf_decode_uri(relative.data());
	relative.resize(std::strlen(relative.c_str()));

	encoded.FilePath = (fs::path(gltf_path).parent_path() / relative).string();
	return true;
}

// Loads the encoded image for `source`, whether it is a plain file or lives in a glTF.
bool FetchImage(const ImageSource& source, EncodedImage& encoded, std::string& error)
{
	if (source.GLTFImageIndex < 0) {
		encoded.FilePath = source.Path;
		return true;
	}

	GltfHandle gltf;

	if (!ParseGltf(source.Path, gltf, error)) {
		return false;
	}

	const bool in_buffer_view = source.GLTFImageIndex < int(gltf.Data->images_count) &&
								gltf.Data->images[source.GLTFImageIndex].buffer_view != nullptr;

	if (in_buffer_view) {
		cgltf_options options = {};
		cgltf_result result = cgltf_load_buffers(&options, gltf.Data, source.Path.c_str());

		if (result != cgltf_result_success) {
			error = "failed to load buffers of '" + source.Path + "': " + GltfResultMessage(result);
			return false;
		}
	}

	return FetchGltfImage(gltf.Data, source.GLTFImageIndex, source.Path, encoded, error);
}

std::string DescribeSource(const ImageSource& source)
{
	if (source.GLTFImageIndex < 0) {
		return source.Path;
	}

	return source.Path + " (image " + std::to_string(source.GLTFImageIndex) + ")";
}

// Decodes to `desired_components` interleaved 8-bit components.
bool Decode(const ImageSource& source, int desired_components, int& width, int& height, std::vector<uint8_t>& pixels,
			std::string& error)
{
	EncodedImage encoded;

	if (!FetchImage(source, encoded, error)) {
		return false;
	}

	int num_components = 0;
	stbi_uc* data = nullptr;

	if (!encoded.FilePath.empty()) {
		data = stbi_load(encoded.FilePath.c_str(), &width, &height, &num_components, desired_components);
	}
	else {
		data = stbi_load_from_memory(encoded.Bytes.data(), int(encoded.Bytes.size()), &width, &height, &num_components,
									 desired_components);
	}

	if (!data) {
		error = "failed to load '" + DescribeSource(source) + "': " + stbi_failure_reason();
		return false;
	}

	pixels.assign(data, data + size_t(width) * height * desired_components);
	stbi_image_free(data);
	return true;
}

// Builds a slot from a glTF texture, if it exists and has a decodable image.
GltfTextureSlot MakeSlot(const cgltf_data* data, const cgltf_texture* texture, const std::string& path, int channel)
{
	GltfTextureSlot slot;

	if (!texture || !texture->image) {
		return slot;
	}

	slot.Source.Path = path;
	slot.Source.GLTFImageIndex = int(cgltf_image_index(data, texture->image));
	slot.Source.Channel = channel;
	return slot;
}

} // namespace

bool IsGltfPath(const std::string& path)
{
	std::string extension = fs::path(path).extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(),
				   [](unsigned char c) { return char(std::tolower(c)); });

	return extension == ".gltf" || extension == ".glb";
}

bool ListGltfMaterials(const std::string& path, std::vector<GltfMaterial>& materials, std::string& error)
{
	materials.clear();

	GltfHandle gltf;

	if (!ParseGltf(path, gltf, error)) {
		return false;
	}

	const cgltf_data* data = gltf.Data;

	for (size_t i = 0; i < data->materials_count; ++i) {
		const cgltf_material& material = data->materials[i];

		GltfMaterial info;
		info.Name = material.name ? material.name : "Material " + std::to_string(i);
		info.Index = int(i);

		if (material.has_pbr_metallic_roughness) {
			const cgltf_pbr_metallic_roughness& pbr = material.pbr_metallic_roughness;

			info.BaseColor = MakeSlot(data, pbr.base_color_texture.texture, path, -1);
			info.Roughness = MakeSlot(data, pbr.metallic_roughness_texture.texture, path, 1);
			info.Metallic = MakeSlot(data, pbr.metallic_roughness_texture.texture, path, 2);
		}

		info.Normal = MakeSlot(data, material.normal_texture.texture, path, -1);
		info.Occlusion = MakeSlot(data, material.occlusion_texture.texture, path, 0);

		materials.push_back(std::move(info));
	}

	// Load buffers once (only if needed) so image sizes can be read from the embedded data.
	bool needs_buffers = false;
	for (size_t i = 0; i < data->images_count; ++i) {
		needs_buffers = needs_buffers || data->images[i].buffer_view != nullptr;
	}

	if (needs_buffers) {
		cgltf_options options = {};
		cgltf_result result = cgltf_load_buffers(&options, gltf.Data, path.c_str());

		if (result != cgltf_result_success) {
			error = "failed to load buffers of '" + path + "': " + GltfResultMessage(result);
			return false;
		}
	}

	for (GltfMaterial& material : materials) {
		for (GltfTextureSlot* slot :
			 { &material.BaseColor, &material.Normal, &material.Occlusion, &material.Roughness, &material.Metallic }) {
			if (slot->Empty()) {
				continue;
			}

			EncodedImage encoded;
			std::string ignored;
			int components = 0;

			if (!FetchGltfImage(data, slot->Source.GLTFImageIndex, path, encoded, ignored)) {
				continue;
			}

			if (!encoded.FilePath.empty()) {
				stbi_info(encoded.FilePath.c_str(), &slot->Width, &slot->Height, &components);
			}
			else {
				stbi_info_from_memory(encoded.Bytes.data(), int(encoded.Bytes.size()), &slot->Width, &slot->Height,
									  &components);
			}
		}
	}

	return true;
}

bool LoadImageRGBA(const ImageSource& source, MPImage& image, std::string& error)
{
	return Decode(source, 4, image.Width, image.Height, image.Pixels, error);
}

bool LoadImageComponent(const ImageSource& source, int& width, int& height, std::vector<uint8_t>& pixels,
						std::string& error)
{
	if (source.Channel < 0) {
		return Decode(source, 1, width, height, pixels, error);
	}

	std::vector<uint8_t> rgba;

	if (!Decode(source, 4, width, height, rgba, error)) {
		return false;
	}

	pixels.resize(size_t(width) * height);

	for (size_t i = 0; i < pixels.size(); ++i) {
		pixels[i] = rgba[i * 4 + std::min(source.Channel, 3)];
	}

	return true;
}

bool GetImageSize(const ImageSource& source, int& width, int& height)
{
	EncodedImage encoded;
	std::string ignored;

	if (!FetchImage(source, encoded, ignored)) {
		return false;
	}

	int components = 0;

	if (!encoded.FilePath.empty()) {
		return stbi_info(encoded.FilePath.c_str(), &width, &height, &components) != 0;
	}

	return stbi_info_from_memory(encoded.Bytes.data(), int(encoded.Bytes.size()), &width, &height, &components) != 0;
}
