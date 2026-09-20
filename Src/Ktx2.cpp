#include "Ktx2.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <ktx.h>

#include "stb_image_resize2.h"

namespace {

constexpr uint32_t VK_FORMAT_R8G8B8A8_UNORM = 37;
constexpr uint32_t VK_FORMAT_R8G8B8A8_SRGB = 43;

} // namespace

Image8 ResizeImage(const Image8& src, int w, int h, ColorSpace space, bool normal_map)
{
    Image8 dst;
    dst.width = w;
    dst.height = h;
    dst.pixels.resize(size_t(w) * h * 4);
    if (space == ColorSpace::SRGB) {
        stbir_resize_uint8_srgb(src.pixels.data(), src.width, src.height, 0, dst.pixels.data(), w, h, 0,
                                STBIR_4CHANNEL);
    } else {
        stbir_resize_uint8_linear(src.pixels.data(), src.width, src.height, 0, dst.pixels.data(), w, h, 0,
                                  STBIR_4CHANNEL);
    }

    if (normal_map) {
        // Averaging shortens vectors; renormalize.
        for (size_t i = 0; i < dst.pixels.size(); i += 4) {
            float x = dst.pixels[i] / 127.5f - 1.0f;
            float y = dst.pixels[i + 1] / 127.5f - 1.0f;
            float z = dst.pixels[i + 2] / 127.5f - 1.0f;
            float len = std::sqrt(x * x + y * y + z * z);
            if (len < 1e-5f) continue;
            const float v[3] = {x / len, y / len, z / len};
            for (int c = 0; c < 3; ++c)
                dst.pixels[i + c] = uint8_t(std::clamp(std::lround((v[c] * 0.5f + 0.5f) * 255.0f), 0L, 255L));
        }
    }
    return dst;
}

std::string WriteKtx2(const std::string& path, const Image8& base, ColorSpace space, bool mipmaps,
                      bool normal_map)
{
    if (base.Empty()) return "empty image";

    std::vector<Image8> levels;
    levels.push_back(base);
    if (mipmaps) {
        while (levels.back().width > 1 || levels.back().height > 1) {
            const Image8& prev = levels.back();
            int w = std::max(1, prev.width / 2);
            int h = std::max(1, prev.height / 2);
            levels.push_back(ResizeImage(prev, w, h, space, normal_map));
        }
    }

    ktxTextureCreateInfo info = {};
    info.vkFormat = space == ColorSpace::SRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    info.baseWidth = uint32_t(base.width);
    info.baseHeight = uint32_t(base.height);
    info.baseDepth = 1;
    info.numDimensions = 2;
    info.numLevels = uint32_t(levels.size());
    info.numLayers = 1;
    info.numFaces = 1;
    info.isArray = KTX_FALSE;
    info.generateMipmaps = KTX_FALSE;

    ktxTexture2* tex = nullptr;
    ktx_error_code_e rc = ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex);
    if (rc != KTX_SUCCESS) return std::string("ktxTexture2_Create failed: ") + ktxErrorString(rc);

    for (uint32_t i = 0; i < levels.size() && rc == KTX_SUCCESS; ++i) {
        rc = ktxTexture_SetImageFromMemory(ktxTexture(tex), i, 0, 0, levels[i].pixels.data(),
                                           levels[i].pixels.size());
    }
    if (rc == KTX_SUCCESS) rc = ktxTexture_WriteToNamedFile(ktxTexture(tex), path.c_str());
    ktxTexture_Destroy(ktxTexture(tex));

    if (rc != KTX_SUCCESS) return "writing '" + path + "' failed: " + ktxErrorString(rc);
    return {};
}

std::string ReadKtx2Levels(const std::string& path, std::vector<Image8>& levels)
{
    levels.clear();
    ktxTexture2* tex = nullptr;
    ktx_error_code_e rc =
        ktxTexture2_CreateFromNamedFile(path.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &tex);
    if (rc != KTX_SUCCESS) return "cannot read '" + path + "': " + ktxErrorString(rc);

    std::string error;
    if (tex->vkFormat != VK_FORMAT_R8G8B8A8_UNORM && tex->vkFormat != VK_FORMAT_R8G8B8A8_SRGB) {
        error = "'" + path + "' is not an RGBA8 texture";
    } else {
        const uint8_t* data = ktxTexture_GetData(ktxTexture(tex));
        for (uint32_t l = 0; l < tex->numLevels && error.empty(); ++l) {
            size_t offset = 0;
            rc = ktxTexture_GetImageOffset(ktxTexture(tex), l, 0, 0, &offset);
            if (rc != KTX_SUCCESS) { error = ktxErrorString(rc); break; }
            Image8 img;
            img.width = int(std::max(1u, tex->baseWidth >> l));
            img.height = int(std::max(1u, tex->baseHeight >> l));
            img.pixels.assign(data + offset, data + offset + size_t(img.width) * img.height * 4);
            levels.push_back(std::move(img));
        }
    }
    ktxTexture_Destroy(ktxTexture(tex));
    return error;
}
