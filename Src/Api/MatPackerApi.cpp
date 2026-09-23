#include "Baker.hpp"
#include "GltfExport.hpp"
#include "ImageSource.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "MatPacker.h"

struct MPGltfMaterials
{
	std::vector<GltfMaterial> Materials;
};

namespace {

thread_local std::string tsLastError;

MPResult Fail(MPResult result, const std::string& message)
{
	tsLastError = message;
	return result;
}

static std::string ToString(const char* text) { return text ? text : ""; }

static ImageSource ToImageSource(const MPImageSource& source)
{
	ImageSource out;

	out.Path = ToString(source.pcPath);
	out.GLTFImageIndex = source.GLTFImageIndex;
	out.Channel = source.Channel;

	return out;
}

static MPImageSource ToMPImageSource(const ImageSource& source)
{
	return { source.Path.c_str(), source.GLTFImageIndex, source.Channel };
}

static BakeSettings ToBakeSettings(const MPBakeSettings& settings)
{
	BakeSettings out;
	out.PathDiffuse = ToImageSource(settings.SourceDiffuse);
	out.PathNormalMap = ToImageSource(settings.SourceNormal);
	out.PathRoughness = ToImageSource(settings.SourceRoughness);
	out.PathMetallic = ToImageSource(settings.SourceMetallic);
	out.PathAO = ToImageSource(settings.SourceOcclusion);

	out.DefaultRoughness = settings.FallbackValues.Roughness;
	out.DefaultMetallic = settings.FallbackValues.Metallic;
	out.DefaultAO = settings.FallbackValues.Occlusion;


	out.OutputDir = ToString(settings.pcOutputDir);
	out.BaseName = ToString(settings.pcBaseName);

	out.bCreateSubfolder = settings.bCreateSubFolder != 0;
	out.bExportMipmaps = settings.bGenerateMipmaps != 0;

	out.ResolutionDivisor = std::clamp(settings.ResolutionDivisor, 1, 16);
	out.Compression = (settings.Compression >= MP_COMPRESSION_NONE && settings.Compression <= MP_COMPRESSION_ASTC_4X4)
						  ? eTextureCompression(settings.Compression)
						  : eTextureCompression::None;
	return out;
}

static GltfExportOptions ToExportOptions(const MPExportOptions* options)
{
	GltfExportOptions out;

	if (options) {
		out.bEmbedTextures = options->bEmbedTexturesInGLTF != 0;
		out.bUseBasisUExtension = options->bUseBasisUExtension != 0;
	}

	return out;
}

static void CopyPath(char (&out)[MP_MAX_PATH], const std::string& path)
{
	const size_t length = path.size() < MP_MAX_PATH ? path.size() : MP_MAX_PATH - 1;
	std::memcpy(out, path.data(), length);
	out[length] = '\0';
}

// Forwards log lines to the caller's callback, remembering the last error for MPLastError().
struct Logger
{
	MPLogFunction Fn;
	void* UserData;
	std::string LastError;

	void operator()(const std::string& line)
	{
		if (line.compare(0, 7, "Error: ") == 0) {
			LastError = line.substr(7);
		}

		if (Fn) {
			Fn(line.c_str(), UserData);
		}
	}
};

static std::string SanitizeFileName(std::string name)
{
	for (char& c : name) {
		if (c == '/' || c == '\\' || c == ':') {
			c = '_';
		}
	}

	return name;
}

} // namespace

extern "C" {

MP_API int MPGetAPIVersion(void) noexcept { return MP_API_VERSION; }
MP_API const char* MPLastError(void) noexcept { return tsLastError.c_str(); }

MP_API void MPInitBakeSettings(MPBakeSettings* settings) noexcept
{
	if (!settings) {
		return;
	}

	*settings = {};
	settings->SourceDiffuse.GLTFImageIndex = -1;
	settings->SourceNormal.GLTFImageIndex = -1;
	settings->SourceRoughness.GLTFImageIndex = -1;
	settings->SourceMetallic.GLTFImageIndex = -1;
	settings->SourceOcclusion.GLTFImageIndex = -1;

	settings->SourceDiffuse.Channel = -1;
	settings->SourceNormal.Channel = -1;
	settings->SourceRoughness.Channel = -1;
	settings->SourceMetallic.Channel = -1;
	settings->SourceOcclusion.Channel = -1;

	settings->FallbackValues.Roughness = 0.5f;
	settings->FallbackValues.Metallic = 0.0f;
	settings->FallbackValues.Occlusion = 1.0f;

	settings->bGenerateMipmaps = true;
	settings->ResolutionDivisor = 1;
	settings->Compression = MP_COMPRESSION_NONE;
}

MP_API void MPInitExportSettings(MPExportOptions* options) noexcept
{
	if (!options) {
		return;
	}

	options->bEmbedTexturesInGLTF = false;
	options->bUseBasisUExtension = true;
}

MP_API MPResult MPBake(const MPBakeSettings* settings, MPBakeOutputs* outputs, MPLogFunction log,
					   void* user_data) noexcept
{
	tsLastError.clear();

	if (!settings) {
		return Fail(MP_INVALID_ARGUMENT, "settings is null");
	}

	Logger logger { log, user_data, {} };
	BakeOutputs baked;

	const bool ok = Bake(ToBakeSettings(*settings), [&](const std::string& line) { logger(line); }, &baked);

	if (outputs) {
		CopyPath(outputs->pPathDiffuse, baked.Diffuse);
		CopyPath(outputs->pPathNormal, baked.Normal);
		CopyPath(outputs->pPathORM, baked.Orm);
	}

	if (!ok) {
		return Fail(MP_ERROR, logger.LastError.empty() ? "bake failed" : logger.LastError);
	}

	return MP_OK;
}

MP_API MPResult MPGLTFMaterialsLoad(const char* path, MPGltfMaterials** materials) noexcept
{
	tsLastError.clear();

	if (!path || !materials) {
		return Fail(MP_INVALID_ARGUMENT, "path or materials is null");
	}

	*materials = nullptr;

	auto* loaded = new (std::nothrow) MPGltfMaterials();

	if (!loaded) {
		return Fail(MP_ERROR, "out of memory");
	}

	std::string error;

	if (!ListGltfMaterials(path, loaded->Materials, error)) {
		delete loaded;
		return Fail(MP_ERROR, error);
	}

	*materials = loaded;
	return MP_OK;
}

MP_API int MPGLTFMaterialsCount(const MPGltfMaterials* materials) noexcept
{
	return materials ? int(materials->Materials.size()) : 0;
}

MP_API MPResult MPGLTFMaterialsGet(const MPGltfMaterials* materials, int index, MPGltfMaterial* material) noexcept
{
	if (!materials || !material || index < 0 || index >= int(materials->Materials.size())) {
		return Fail(MP_INVALID_ARGUMENT, "materials or material is null, or index is out of range");
	}

	const GltfMaterial& source = materials->Materials[size_t(index)];

	auto GetTextureSlot = [](const GltfTextureSlot& slot)
	{ return MPTextureSlot { ToMPImageSource(slot.Source), slot.Width, slot.Height }; };

	material->pcName = source.Name.c_str();
	material->MaterialIndex = source.Index;
	material->BaseColor = GetTextureSlot(source.BaseColor);
	material->Normal = GetTextureSlot(source.Normal);
	material->Occlusion = GetTextureSlot(source.Occlusion);
	material->Roughness = GetTextureSlot(source.Roughness);
	material->Metallic = GetTextureSlot(source.Metallic);

	return MP_OK;
}

MP_API void MPGLTFMaterialsFree(MPGltfMaterials* materials) noexcept { delete materials; }

MP_API MPResult MPGLTFExport(const char* source_path, const char* output_path, const MPMaterialLink* links,
							 int link_count, const MPExportOptions* options, MPLogFunction log,
							 void* user_data) noexcept
{
	tsLastError.clear();

	if (!source_path || !output_path || (link_count > 0 && !links) || link_count < 0) {
		return Fail(MP_INVALID_ARGUMENT, "source_path, output_path or links is null");
	}

	std::vector<BakedMaterialLink> converted;

	for (int i = 0; i < link_count; i++) {
		BakedMaterialLink link;

		link.MaterialIndex = links[i].MaterialIndex;
		link.BaseColor = ToString(links[i].pcBaseColor);
		link.Normal = ToString(links[i].pcNormal);
		link.MetallicRoughness = ToString(links[i].pcMetallicRoughness);
		link.Occlusion = ToString(links[i].pcOcclusion);

		converted.push_back(std::move(link));
	}

	Logger logger { log, user_data, {} };
	std::string error;

	if (!ExportGltfWithBakedMaterials(
			source_path, output_path, converted, ToExportOptions(options),
			[&](const std::string& line) { logger(line); }, error)) {
		return Fail(MP_ERROR, error);
	}

	return MP_OK;
}

MP_API MPResult MPGLTFBake(const char* source_path, const char* output_path, const MPBakeSettings* settings,
						   const MPExportOptions* options, MPLogFunction log, void* user_data) noexcept
{
	tsLastError.clear();

	if (!source_path || !output_path || !settings) {
		return Fail(MP_INVALID_ARGUMENT, "source_path, output_path or settings is null");
	}

	std::vector<GltfMaterial> materials;
	std::string error;

	if (!ListGltfMaterials(source_path, materials, error)) {
		return Fail(MP_ERROR, error);
	}

	Logger logger { log, user_data, {} };
	std::vector<BakedMaterialLink> links;
	bool all_ok = true;

	for (const GltfMaterial& material : materials) {
		if (material.BaseColor.Empty() && material.Normal.Empty() && material.Occlusion.Empty() &&
			material.Roughness.Empty() && material.Metallic.Empty()) {
			continue;
		}

		logger("== " + material.Name + " ==");

		BakeSettings bake_settings = ToBakeSettings(*settings);
		bake_settings.PathDiffuse = material.BaseColor.Source;
		bake_settings.PathNormalMap = material.Normal.Source;
		bake_settings.PathRoughness = material.Roughness.Source;
		bake_settings.PathMetallic = material.Metallic.Source;
		bake_settings.PathAO = material.Occlusion.Source;
		bake_settings.BaseName = SanitizeFileName(material.Name);

		BakeOutputs baked;
		all_ok = Bake(bake_settings, [&](const std::string& line) { logger(line); }, &baked) && all_ok;

		// Link only the slots the material has a source for, so a slot never picks up the fallback values.
		BakedMaterialLink link;
		link.MaterialIndex = material.Index;
		link.BaseColor = baked.Diffuse;
		link.Normal = baked.Normal;
		link.MetallicRoughness = material.Roughness.Empty() && material.Metallic.Empty() ? std::string() : baked.Orm;
		link.Occlusion = material.Occlusion.Empty() ? std::string() : baked.Orm;
		links.push_back(std::move(link));
	}

	if (links.empty()) {
		return Fail(MP_ERROR, "'" + std::string(source_path) + "' has no materials with textures");
	}

	if (!ExportGltfWithBakedMaterials(
			source_path, output_path, links, ToExportOptions(options), [&](const std::string& line) { logger(line); },
			error)) {
		return Fail(MP_ERROR, error);
	}

	if (!all_ok) {
		return Fail(MP_ERROR, logger.LastError.empty() ? "some materials failed to bake" : logger.LastError);
	}

	return MP_OK;
}

} // extern "C"
