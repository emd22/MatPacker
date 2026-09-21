#include "Baker.hpp"

#include "Ktx2.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "stb_image_resize2.h"

namespace {

struct LoadedImage
{
	Image8 RGBAImage;
	bool bIsOK = false;
};

LoadedImage LoadRGBAImage(const ImageSource& source, std::string& error)
{
	LoadedImage loaded_image;
	loaded_image.bIsOK = LoadImageRGBA(source, loaded_image.RGBAImage, error);
	return loaded_image;
}

// A single 8-bit channel (grayscale) source, optionally resized.
struct GrayscaleImage
{
	int Width = 0;
	int Height = 0;

	std::vector<uint8_t> Pixels;
};

bool LoadGray(const ImageSource& source, GrayscaleImage& g, std::string& error)
{
	return LoadImageComponent(source, g.Width, g.Height, g.Pixels, error);
}

uint8_t ToByte(float v) { return uint8_t(std::clamp(std::lround(v * 255.0f), 0L, 255L)); }

} // namespace

bool Bake(const BakeSettings& bake_settings, const std::function<void(const std::string&)>& log,
		  std::vector<std::string>* written)
{
	namespace fs = std::filesystem;

	if (bake_settings.OutputDir.empty()) {
		log("Error: no output directory selected.");
		return false;
	}
	if (bake_settings.BaseName.empty()) {
		log("Error: no output base name given.");
		return false;
	}

	const fs::path target_dir = bake_settings.bCreateSubfolder
									? fs::path(bake_settings.OutputDir) / bake_settings.BaseName
									: fs::path(bake_settings.OutputDir);

	std::error_code ec;
	fs::create_directories(target_dir, ec);
	if (ec) {
		log("Error: cannot create '" + target_dir.string() + "': " + ec.message());
		return false;
	}

	auto MakeOutputPath = [&](const char* suffix)
	{ return (target_dir / (bake_settings.BaseName + suffix + ".ktx2")).string(); };

	bool all_ok = true;
	int diffuse_w = 0, diffuse_h = 0;
	std::string err;

	auto OutputImage = [&](const std::string& path, const Image8& img, ColorSpace cs, bool normal)
	{
		Image8 scaled;
		if (bake_settings.ResolutionDivisor > 1) {
			scaled = ResizeImage(img, std::max(1, img.width / bake_settings.ResolutionDivisor),
								 std::max(1, img.height / bake_settings.ResolutionDivisor), cs, normal);
		}

		const Image8& out_img = bake_settings.ResolutionDivisor > 1 ? scaled : img;

		std::string e = WriteKtx2(path, out_img, cs, bake_settings.bExportMipmaps, normal);
		if (e.empty()) {
			if (written) {
				written->push_back(path);
			}

			log("Wrote " + path + " (" + std::to_string(out_img.width) + "x" + std::to_string(out_img.height) + ")");
		}
		else {
			log("Error: " + e);
			all_ok = false;
		}
	};

	// Image 1, diffuse / base color (sRGB)
	if (!bake_settings.PathDiffuse.Empty()) {
		LoadedImage l = LoadRGBAImage(bake_settings.PathDiffuse, err);

		if (!l.bIsOK) {
			log("Error: " + err);
			all_ok = false;
		}
		else {
			diffuse_w = l.RGBAImage.width;
			diffuse_h = l.RGBAImage.height;
			OutputImage(MakeOutputPath("_diffuse"), l.RGBAImage, ColorSpace::SRGB, false);
		}
	}
	else {
		log("No diffuse image; skipping diffuse.");
	}

	// Image 2, normal map (linear)
	if (!bake_settings.PathNormalMap.Empty()) {
		LoadedImage l = LoadRGBAImage(bake_settings.PathNormalMap, err);
		if (!l.bIsOK) {
			log("Error: " + err);
			all_ok = false;
		}
		else {
			for (size_t i = 3; i < l.RGBAImage.pixels.size(); i += 4) {
				l.RGBAImage.pixels[i] = 255;
			}

			OutputImage(MakeOutputPath("_normal"), l.RGBAImage, ColorSpace::Linear, true);
		}
	}
	else {
		log("No normal map; skipping normal.");
	}

	// Image 3,  R = AO, G = Roughness, B = Metallic, A unused.
	// Channel order (R, G, B) is the ORM convention: Occlusion, Roughness, Metallic.
	const ImageSource* paths[3] = { &bake_settings.PathAO, &bake_settings.PathRoughness, &bake_settings.PathMetallic };
	const float defaults[3] = { bake_settings.DefaultAO, bake_settings.DefaultRoughness,
								bake_settings.DefaultMetallic };
	const char* names[3] = { "AO", "roughness", "metallic" };

	GrayscaleImage f3_channels[3];
	bool has_components[3] = { false, false, false };

	int width = 0, height = 0;

	for (int component_index = 0; component_index < 3; component_index++) {
		if (paths[component_index]->Empty()) {
			continue;
		}

		if (!LoadGray(*paths[component_index], f3_channels[component_index], err)) {
			log("Error: " + err);
			all_ok = false;
			continue;
		}

		has_components[component_index] = true;

		if (f3_channels[component_index].Width * f3_channels[component_index].Height > width * height) {
			width = f3_channels[component_index].Width;
			height = f3_channels[component_index].Height;
		}
	}

	if (width == 0) {
		width = diffuse_w ? diffuse_w : 1;
		height = diffuse_h ? diffuse_h : 1;
	}

	Image8 orm;
	orm.width = width;
	orm.height = height;
	orm.pixels.resize(size_t(width) * height * 4);

	for (int component_index = 0; component_index < 3; component_index++) {
		std::vector<uint8_t> plane;

		if (has_components[component_index]) {
			const GrayscaleImage& g = f3_channels[component_index];

			if (g.Width == width && g.Height == height) {
				plane = g.Pixels;
			}
			else {
				plane.resize(size_t(width) * height);
				stbir_resize_uint8_linear(g.Pixels.data(), g.Width, g.Height, 0, plane.data(), width, height, 0,
										  STBIR_1CHANNEL);
				log(std::string("Resized ") + names[component_index] + " to " + std::to_string(width) + "x" +
					std::to_string(height));
			}
		}
		else {
			plane.assign(size_t(width) * height, ToByte(defaults[component_index]));

			log(std::string("No ") + names[component_index] + " map; using default " +
				std::to_string(defaults[component_index]));
		}

		for (size_t i = 0; i < plane.size(); i++) {
			orm.pixels[i * 4 + component_index] = plane[i];
		}
	}

	for (size_t i = 3; i < orm.pixels.size(); i += 4) {
		orm.pixels[i] = 255;
	}

	OutputImage(MakeOutputPath("_orm"), orm, ColorSpace::Linear, false);
	return all_ok;
}
