#include "Ktx2.hpp"

#include <ktx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "stb_image_resize2.h"

namespace {

constexpr uint32_t VK_FORMAT_R8G8B8A8_UNORM = 37;
constexpr uint32_t VK_FORMAT_R8G8B8A8_SRGB = 43;

// Zstd level for supercompression: high ratio without taking long on large textures.
constexpr uint32_t spcZstdLevel = 15;

} // namespace

MPImage ResizeImage(const MPImage& src, int w, int h, eColorSpace space, bool normal_map)
{
	MPImage dst;
	dst.Width = w;
	dst.Height = h;
	dst.Pixels.resize(size_t(w) * h * 4);
	if (space == eColorSpace::SRGB) {
		stbir_resize_uint8_srgb(src.Pixels.data(), src.Width, src.Height, 0, dst.Pixels.data(), w, h, 0,
								STBIR_4CHANNEL);
	}
	else {
		stbir_resize_uint8_linear(src.Pixels.data(), src.Width, src.Height, 0, dst.Pixels.data(), w, h, 0,
								  STBIR_4CHANNEL);
	}

	if (normal_map) {
		// Averaging shortens vectors; renormalize.
		for (size_t i = 0; i < dst.Pixels.size(); i += 4) {
			float x = dst.Pixels[i] / 127.5f - 1.0f;
			float y = dst.Pixels[i + 1] / 127.5f - 1.0f;
			float z = dst.Pixels[i + 2] / 127.5f - 1.0f;
			float len = std::sqrt(x * x + y * y + z * z);
			if (len < 1e-5f)
				continue;
			const float v[3] = { x / len, y / len, z / len };
			for (int c = 0; c < 3; ++c)
				dst.Pixels[i + c] = uint8_t(std::clamp(std::lround((v[c] * 0.5f + 0.5f) * 255.0f), 0L, 255L));
		}
	}
	return dst;
}

std::string WriteKTX2(const std::string& path, const MPImage& base, eColorSpace space, bool mipmaps, bool normal_map,
					  eTextureCompression compression)
{
	if (base.IsEmpty())
		return "empty image";

	std::vector<MPImage> levels;
	levels.push_back(base);
	if (mipmaps) {
		while (levels.back().Width > 1 || levels.back().Height > 1) {
			const MPImage& prev = levels.back();
			int width = std::max(1, prev.Width / 2);
			int height = std::max(1, prev.Height / 2);
			levels.push_back(ResizeImage(prev, width, height, space, normal_map));
		}
	}

	ktxTextureCreateInfo info = {};
	info.vkFormat = space == eColorSpace::SRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
	info.baseWidth = uint32_t(base.Width);
	info.baseHeight = uint32_t(base.Height);
	info.baseDepth = 1;
	info.numDimensions = 2;
	info.numLevels = uint32_t(levels.size());
	info.numLayers = 1;
	info.numFaces = 1;
	info.isArray = KTX_FALSE;
	info.generateMipmaps = KTX_FALSE;

	ktxTexture2* tex = nullptr;
	ktx_error_code_e rc = ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex);
	if (rc != KTX_SUCCESS)
		return std::string("ktxTexture2_Create failed: ") + ktxErrorString(rc);

	for (uint32_t i = 0; i < levels.size() && rc == KTX_SUCCESS; ++i) {
		rc = ktxTexture_SetImageFromMemory(ktxTexture(tex), i, 0, 0, levels[i].Pixels.data(), levels[i].Pixels.size());
	}

	const uint32_t thread_count = std::max(1u, std::thread::hardware_concurrency());

	if (rc == KTX_SUCCESS) {
		switch (compression) {
		case eTextureCompression::None:
			break;

		case eTextureCompression::Zstd:
			rc = ktxTexture2_DeflateZstd(tex, spcZstdLevel);
			break;

		case eTextureCompression::BasisUastc: {
			ktxBasisParams params = {};
			params.structSize = sizeof(params);
			params.uastc = KTX_TRUE;
			params.uastcFlags = KTX_PACK_UASTC_LEVEL_DEFAULT;
			params.compressionLevel = KTX_ETC1S_DEFAULT_COMPRESSION_LEVEL; // required by the API even for UASTC
			params.threadCount = thread_count;
			// Tunes the encoder for normal vectors and marks the texture as a normal map (linear data only).
			params.normalMap = normal_map ? KTX_TRUE : KTX_FALSE;

			rc = ktxTexture2_CompressBasisEx(tex, &params);

			// UASTC compresses well under Zstd, and readers expect it.
			if (rc == KTX_SUCCESS)
				rc = ktxTexture2_DeflateZstd(tex, spcZstdLevel);
			break;
		}

		case eTextureCompression::Astc4x4: {
			ktxAstcParams params = {};
			params.structSize = sizeof(params);
			params.blockDimension = KTX_PACK_ASTC_BLOCK_DIMENSION_4x4;
			params.mode = KTX_PACK_ASTC_ENCODER_MODE_LDR;
			params.qualityLevel = KTX_PACK_ASTC_QUALITY_LEVEL_MEDIUM;
			params.threadCount = thread_count;
			params.perceptual = space == eColorSpace::SRGB ? KTX_TRUE : KTX_FALSE;

			rc = ktxTexture2_CompressAstcEx(tex, &params);
			break;
		}
		}
	}

	if (rc == KTX_SUCCESS)
		rc = ktxTexture_WriteToNamedFile(ktxTexture(tex), path.c_str());
	ktxTexture_Destroy(ktxTexture(tex));

	if (rc != KTX_SUCCESS)
		return "writing '" + path + "' failed: " + ktxErrorString(rc);
	return {};
}

std::string ReadKTX2Levels(const std::string& path, std::vector<MPImage>& levels)
{
	levels.clear();
	ktxTexture2* tex = nullptr;
	ktx_error_code_e rc = ktxTexture2_CreateFromNamedFile(path.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &tex);
	if (rc != KTX_SUCCESS)
		return "cannot read '" + path + "': " + ktxErrorString(rc);

	// Basis Universal files carry no RGBA data until transcoded; do that so they can be previewed.
	if (ktxTexture2_NeedsTranscoding(tex)) {
		rc = ktxTexture2_TranscodeBasis(tex, KTX_TTF_RGBA32, 0);
		if (rc != KTX_SUCCESS) {
			ktxTexture_Destroy(ktxTexture(tex));
			return "cannot transcode '" + path + "': " + ktxErrorString(rc);
		}
	}

	std::string error;
	if (tex->vkFormat != VK_FORMAT_R8G8B8A8_UNORM && tex->vkFormat != VK_FORMAT_R8G8B8A8_SRGB) {
		error = "'" + path + "' uses a GPU-compressed format (e.g. ASTC) that cannot be previewed here";
	}
	else {
		const uint8_t* data = ktxTexture_GetData(ktxTexture(tex));
		for (uint32_t l = 0; l < tex->numLevels && error.empty(); ++l) {
			size_t offset = 0;
			rc = ktxTexture_GetImageOffset(ktxTexture(tex), l, 0, 0, &offset);
			if (rc != KTX_SUCCESS) {
				error = ktxErrorString(rc);
				break;
			}
			MPImage img;
			img.Width = int(std::max(1u, tex->baseWidth >> l));
			img.Height = int(std::max(1u, tex->baseHeight >> l));
			img.Pixels.assign(data + offset, data + offset + size_t(img.Width) * img.Height * 4);
			levels.push_back(std::move(img));
		}
	}
	ktxTexture_Destroy(ktxTexture(tex));
	return error;
}
