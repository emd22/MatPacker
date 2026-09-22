#include "GltfExport.hpp"

#include "Json.hpp"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

#include "cgltf.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t spcGlbMagic = 0x46546C67;	 // "glTF"
constexpr uint32_t spcGlbChunkJson = 0x4E4F534A; // "JSON"
constexpr uint32_t spcGlbChunkBin = 0x004E4942;	 // "BIN\0"

constexpr const char* spcBasisuExtension = "KHR_texture_basisu";

bool HasExtension(const std::string& path, const char* extension)
{
	std::string ext = fs::path(path).extension().string();

	for (char& c : ext) {
		c = char(std::tolower(static_cast<unsigned char>(c)));
	}

	return ext == extension;
}

bool ReadFile(const std::string& path, std::string& out, std::string& error)
{
	std::ifstream file(fs::u8path(path), std::ios::binary);

	if (!file) {
		error = "cannot open '" + path + "'";
		return false;
	}

	out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	return true;
}

bool WriteFile(const std::string& path, const std::string& data, std::string& error)
{
	std::ofstream file(fs::u8path(path), std::ios::binary | std::ios::trunc);

	if (!file || !file.write(data.data(), std::streamsize(data.size()))) {
		error = "cannot write '" + path + "'";
		return false;
	}

	return true;
}

uint32_t ReadU32(const std::string& data, size_t offset)
{
	uint32_t value = 0;
	std::memcpy(&value, data.data() + offset, sizeof(value));
	return value; // glb is little endian, as are all supported hosts
}

void AppendU32(std::string& out, uint32_t value) { out.append(reinterpret_cast<const char*>(&value), sizeof(value)); }

// Splits a .glb into its JSON text and (optional) binary chunk.
bool ReadGlb(const std::string& data, std::string& json, std::string& bin, std::string& error)
{
	if (data.size() < 20 || ReadU32(data, 0) != spcGlbMagic) {
		error = "not a binary glTF file";
		return false;
	}

	size_t offset = 12;
	bool has_json = false;

	while (offset + 8 <= data.size()) {
		const uint32_t length = ReadU32(data, offset);
		const uint32_t type = ReadU32(data, offset + 4);
		offset += 8;

		if (offset + length > data.size()) {
			error = "truncated glb chunk";
			return false;
		}

		if (type == spcGlbChunkJson && !has_json) {
			json = data.substr(offset, length);
			has_json = true;
		}
		else if (type == spcGlbChunkBin && bin.empty()) {
			bin = data.substr(offset, length);
		}

		offset += length;
	}

	if (!has_json) {
		error = "glb has no JSON chunk";
		return false;
	}

	return true;
}

std::string MakeGlb(std::string json, std::string bin)
{
	// Chunks are 4-byte aligned: JSON is padded with spaces, binary data with zeros.
	json.append((4 - json.size() % 4) % 4, ' ');
	bin.append((4 - bin.size() % 4) % 4, '\0');

	const size_t total = 12 + 8 + json.size() + (bin.empty() ? 0 : 8 + bin.size());

	std::string out;
	out.reserve(total);
	AppendU32(out, spcGlbMagic);
	AppendU32(out, 2);
	AppendU32(out, uint32_t(total));

	AppendU32(out, uint32_t(json.size()));
	AppendU32(out, spcGlbChunkJson);
	out += json;

	if (!bin.empty()) {
		AppendU32(out, uint32_t(bin.size()));
		AppendU32(out, spcGlbChunkBin);
		out += bin;
	}

	return out;
}

// True for a URI that is a path relative to the glTF (not a data URI, absolute path or other scheme).
bool IsRelativeUri(const std::string& uri)
{
	if (uri.empty() || uri[0] == '/' || uri.compare(0, 5, "data:") == 0) {
		return false;
	}

	const size_t colon = uri.find(':');
	return colon == std::string::npos || uri.find('/') < colon;
}

std::string DecodeUri(std::string uri)
{
	cgltf_decode_uri(uri.data());
	uri.resize(std::strlen(uri.c_str()));
	return uri;
}

std::string EncodeUri(const std::string& path)
{
	static const char* spcHex = "0123456789ABCDEF";
	std::string out;

	for (const char ch : path) {
		const unsigned char c = static_cast<unsigned char>(ch);

		if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/') {
			out += ch;
		}
		else {
			out += '%';
			out += spcHex[c >> 4];
			out += spcHex[c & 0xF];
		}
	}

	return out;
}

// A URI for `target` as seen from the folder `from_dir`: relative when possible, otherwise absolute.
std::string MakeUri(const fs::path& target, const fs::path& from_dir)
{
	const fs::path absolute_target = fs::absolute(target).lexically_normal();
	const fs::path relative = absolute_target.lexically_relative(fs::absolute(from_dir).lexically_normal());

	return EncodeUri((relative.empty() ? absolute_target : relative).generic_u8string());
}

// Rewrites the relative URIs in `array` (buffers or images) so they resolve from `to_dir` instead of `from_dir`.
void RebaseUris(JsonValue* array, const fs::path& from_dir, const fs::path& to_dir)
{
	if (!array || !array->IsArray()) {
		return;
	}

	for (JsonValue& entry : array->Array) {
		JsonValue* uri = entry.Find("uri");

		if (uri && uri->IsString() && IsRelativeUri(uri->Text)) {
			uri->Text = MakeUri(from_dir / fs::u8path(DecodeUri(uri->Text)), to_dir);
		}
	}
}

JsonValue& EnsureArray(JsonValue& root, const char* key)
{
	JsonValue& array = root[key];

	if (!array.IsArray()) {
		array = JsonValue::MakeArray();
	}

	return array;
}

void AddExtensionName(JsonValue& root, const char* list, const char* name)
{
	JsonValue& names = EnsureArray(root, list);

	for (const JsonValue& existing : names.Array) {
		if (existing.IsString() && existing.Text == name) {
			return;
		}
	}

	names.Array.push_back(JsonValue::MakeString(name));
}

std::string EncodeBase64(const std::string& data)
{
	static const char* spcAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((data.size() + 2) / 3 * 4);

	for (size_t i = 0; i < data.size(); i += 3) {
		const size_t remaining = data.size() - i;
		uint32_t triple = uint32_t(static_cast<unsigned char>(data[i])) << 16;

		if (remaining > 1) {
			triple |= uint32_t(static_cast<unsigned char>(data[i + 1])) << 8;
		}
		if (remaining > 2) {
			triple |= uint32_t(static_cast<unsigned char>(data[i + 2]));
		}

		out += spcAlphabet[(triple >> 18) & 0x3F];
		out += spcAlphabet[(triple >> 12) & 0x3F];
		out += remaining > 1 ? spcAlphabet[(triple >> 6) & 0x3F] : '=';
		out += remaining > 2 ? spcAlphabet[triple & 0x3F] : '=';
	}

	return out;
}

/**
 * @brief Makes sure buffers[0] has no URI, so it can hold a glb's binary chunk. If the first buffer is an external
 * file, a new buffer is put in front of it and every buffer view is renumbered to match.
 */
void EnsureGlbBuffer(JsonValue& root)
{
	JsonValue& buffers = EnsureArray(root, "buffers");

	if (!buffers.Array.empty() && !buffers.Array[0].Find("uri")) {
		return;
	}

	JsonValue buffer = JsonValue::MakeObject();
	buffer["byteLength"] = JsonValue::MakeNumber(0);
	buffers.Array.insert(buffers.Array.begin(), std::move(buffer));

	JsonValue* views = root.Find("bufferViews");

	if (!views || !views->IsArray()) {
		return;
	}

	auto Renumber = [](JsonValue* index)
	{
		if (index && index->IsNumber()) {
			*index = JsonValue::MakeNumber(index->AsInt() + 1);
		}
	};

	for (JsonValue& view : views->Array) {
		Renumber(view.Find("buffer"));

		// Meshopt-compressed views name the buffer holding their compressed data too.
		if (JsonValue* extensions = view.Find("extensions")) {
			for (const char* name : { "EXT_meshopt_compression", "KHR_meshopt_compression" }) {
				if (JsonValue* meshopt = extensions->Find(name)) {
					Renumber(meshopt->Find("buffer"));
				}
			}
		}
	}
}

/**
 * @brief Adds images and textures for baked .ktx2 files, reusing them when the same file (and sampler) is linked
 * more than once, such as the ORM texture for both occlusion and metallic-roughness.
 * Images are referenced by URI, or embedded: into the glb's binary chunk when `bin` and `buffer_views` are given,
 * otherwise as base64 data URIs.
 */
class TextureLinker
{
public:
	TextureLinker(JsonValue& images, JsonValue& textures, const fs::path& output_dir, bool embed,
				  std::string* bin = nullptr, JsonValue* buffer_views = nullptr)
		: mImages(images), mTextures(textures), mOutputDir(output_dir), mbEmbed(embed), mpBin(bin),
		  mpBufferViews(buffer_views)
	{
	}

	// Points the texture info `slot` (e.g. material.normalTexture) at `file`, keeping its other properties.
	bool Link(JsonValue& slot, const std::string& file, std::string& error)
	{
		if (!slot.IsObject()) {
			slot = JsonValue::MakeObject();
		}

		// Keep whatever sampler the replaced texture used.
		long long sampler = -1;

		if (const JsonValue* index = slot.Find("index")) {
			const long long old = index->AsInt();

			if (old >= 0 && size_t(old) < mTextures.Array.size()) {
				if (const JsonValue* s = mTextures.Array[size_t(old)].Find("sampler")) {
					sampler = s->AsInt();
				}
			}
		}

		long long image = -1;

		if (!ImageFor(file, image, error)) {
			return false;
		}

		slot["index"] = JsonValue::MakeNumber(TextureFor(image, sampler));
		return true;
	}

private:
	bool ImageFor(const std::string& file, long long& index, std::string& error)
	{
		const std::string uri = MakeUri(fs::u8path(file), mOutputDir);
		const auto it = mImageByUri.find(uri);

		if (it != mImageByUri.end()) {
			index = it->second;
			return true;
		}

		JsonValue image = JsonValue::MakeObject();

		if (mbEmbed) {
			std::string bytes;

			if (!ReadFile(file, bytes, error)) {
				return false;
			}

			if (mpBin && mpBufferViews) {
				mpBin->append((4 - mpBin->size() % 4) % 4, '\0');

				JsonValue view = JsonValue::MakeObject();
				view["buffer"] = JsonValue::MakeNumber(0);
				view["byteOffset"] = JsonValue::MakeNumber((long long)mpBin->size());
				view["byteLength"] = JsonValue::MakeNumber((long long)bytes.size());

				mpBin->append(bytes);
				mpBufferViews->Array.push_back(std::move(view));
				image["bufferView"] = JsonValue::MakeNumber((long long)mpBufferViews->Array.size() - 1);
			}
			else {
				image["uri"] = JsonValue::MakeString("data:image/ktx2;base64," + EncodeBase64(bytes));
			}
		}
		else {
			image["uri"] = JsonValue::MakeString(uri);
		}

		image["mimeType"] = JsonValue::MakeString("image/ktx2");
		image["name"] = JsonValue::MakeString(fs::u8path(file).stem().u8string());

		mImages.Array.push_back(std::move(image));
		index = mImageByUri[uri] = (long long)(mImages.Array.size() - 1);
		return true;
	}

	long long TextureFor(long long image, long long sampler)
	{
		const auto key = std::make_pair(image, sampler);
		const auto it = mTextureByImage.find(key);

		if (it != mTextureByImage.end()) {
			return it->second;
		}

		JsonValue texture = JsonValue::MakeObject();

		if (sampler >= 0) {
			texture["sampler"] = JsonValue::MakeNumber(sampler);
		}

		texture["extensions"][spcBasisuExtension]["source"] = JsonValue::MakeNumber(image);

		mTextures.Array.push_back(std::move(texture));
		return mTextureByImage[key] = (long long)(mTextures.Array.size() - 1);
	}

	JsonValue& mImages;
	JsonValue& mTextures;
	fs::path mOutputDir;

	bool mbEmbed = false;
	std::string* mpBin = nullptr;
	JsonValue* mpBufferViews = nullptr;

	std::map<std::string, long long> mImageByUri;
	std::map<std::pair<long long, long long>, long long> mTextureByImage;
};

} // namespace

bool ExportGltfWithBakedMaterials(const std::string& source_path, const std::string& output_path,
								  const std::vector<BakedMaterialLink>& links, bool embed_textures,
								  const std::function<void(const std::string&)>& log, std::string& error)
{
	const fs::path source_dir = fs::absolute(fs::u8path(source_path)).parent_path();
	const fs::path output_dir = fs::absolute(fs::u8path(output_path)).parent_path();
	const bool output_glb = HasExtension(output_path, ".glb");
	const bool embed_in_bin = embed_textures && output_glb;

	std::string data, json_text, bin;

	if (!ReadFile(source_path, data, error)) {
		return false;
	}

	if (data.size() >= 4 && ReadU32(data, 0) == spcGlbMagic) {
		if (!ReadGlb(data, json_text, bin, error)) {
			error = "'" + source_path + "': " + error;
			return false;
		}
	}
	else {
		json_text = std::move(data);
	}

	JsonValue root;

	if (!ParseJson(json_text, root, error) || !root.IsObject()) {
		error = "'" + source_path + "' is not valid glTF JSON" + (error.empty() ? "" : ": " + error);
		return false;
	}

	RebaseUris(root.Find("buffers"), source_dir, output_dir);
	RebaseUris(root.Find("images"), source_dir, output_dir);

	// Add every top-level member before taking pointers into the root, as adding one can move the others.
	EnsureArray(root, "images");
	EnsureArray(root, "textures");

	if (embed_in_bin) {
		EnsureGlbBuffer(root);
		EnsureArray(root, "bufferViews");
	}

	JsonValue* materials = root.Find("materials");
	TextureLinker linker(*root.Find("images"), *root.Find("textures"), output_dir, embed_textures,
						 embed_in_bin ? &bin : nullptr, embed_in_bin ? root.Find("bufferViews") : nullptr);
	int linked = 0;

	for (const BakedMaterialLink& link : links) {
		if (!materials || !materials->IsArray() || link.MaterialIndex < 0 ||
			size_t(link.MaterialIndex) >= materials->Array.size()) {
			log("Warning: material " + std::to_string(link.MaterialIndex) + " is not in the glTF; skipped.");
			continue;
		}

		JsonValue& material = materials->Array[size_t(link.MaterialIndex)];

		if (!material.IsObject()) {
			material = JsonValue::MakeObject();
		}

		const bool ok =
			(link.BaseColor.empty() ||
			 linker.Link(material["pbrMetallicRoughness"]["baseColorTexture"], link.BaseColor, error)) &&
			(link.MetallicRoughness.empty() ||
			 linker.Link(material["pbrMetallicRoughness"]["metallicRoughnessTexture"], link.MetallicRoughness,
						 error)) &&
			(link.Normal.empty() || linker.Link(material["normalTexture"], link.Normal, error)) &&
			(link.Occlusion.empty() || linker.Link(material["occlusionTexture"], link.Occlusion, error));

		if (!ok) {
			return false;
		}

		linked++;
	}

	if (linked == 0) {
		error = "no materials were linked";
		return false;
	}

	// The textures have no fallback image, so readers must support KTX2 to show them.
	AddExtensionName(root, "extensionsUsed", spcBasisuExtension);
	AddExtensionName(root, "extensionsRequired", spcBasisuExtension);

	std::error_code ec;
	fs::create_directories(output_dir, ec);

	if (output_glb) {
		if (embed_in_bin) {
			root["buffers"].Array[0]["byteLength"] = JsonValue::MakeNumber((long long)bin.size());
		}

		if (!WriteFile(output_path, MakeGlb(WriteJson(root, false), bin), error)) {
			return false;
		}
	}
	else {
		// A .gltf cannot embed the glb's binary chunk, so it moves to a .bin beside the output.
		if (!bin.empty()) {
			JsonValue* buffers = root.Find("buffers");

			if (!buffers || !buffers->IsArray() || buffers->Array.empty()) {
				error = "the glb has binary data but no buffer for it";
				return false;
			}

			fs::path bin_path = fs::u8path(output_path);
			bin_path.replace_extension(".bin");

			if (!WriteFile(bin_path.u8string(), bin, error)) {
				return false;
			}

			buffers->Array[0]["uri"] = JsonValue::MakeString(EncodeUri(bin_path.filename().u8string()));
			log("Wrote " + bin_path.u8string());
		}

		if (!WriteFile(output_path, WriteJson(root, true), error)) {
			return false;
		}
	}

	log("Wrote " + output_path + " (" + std::to_string(linked) + " material" + (linked == 1 ? "" : "s") + " " +
		(embed_textures ? "with embedded baked textures" : "linked to baked textures") + ")");
	return true;
}
