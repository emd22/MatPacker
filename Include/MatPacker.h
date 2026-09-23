/**
 * MatPacker (as a lib)
 *
 * A plain C API, so it can be loaded into multiple languages. Strings are UTF-8. Functions that can fail return
 * an MPResult; the reason for the last failure on the calling thread is available from MPLastError(). Progress and
 * warnings go to an optional log callback. No function throws: C++ callers see them as noexcept.
 */
#ifndef MATPACKER_H
#define MATPACKER_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#if defined(MATPACKER_BUILDING)
#define MP_API __declspec(dllexport)
#else
#define MP_API __declspec(dllimport)
#endif
#else
#define MP_API __attribute__((visibility("default")))
#endif

#if defined(__cplusplus)
#define MP_NOEXCEPT noexcept
#else
#define MP_NOEXCEPT
#endif

/** Bump when changing structs or functions signatures */
#define MP_API_VERSION 1

typedef int MPBool32;

typedef enum MPResult
{
	MP_OK = 0,
	MP_ERROR = 1,
	MP_INVALID_ARGUMENT = 2,
} MPResult;

/**
 * @brief Compression technique for the exported KTX images.
 */
typedef enum MPCompression
{
	/// Raw RGBA8
	MP_COMPRESSION_NONE = 0,

	/// Raw RGBA8, Zstd-compressed on disk (lossless)
	MP_COMPRESSION_ZSTD = 1,

	/// UASTC + Zstd; transcoded to BC7 / ASTC when loaded (lossy)
	MP_COMPRESSION_BASIS_UASTC = 2,

	/// ASTC 4x4 LDR (lossy, mainly mobile GPUs)
	MP_COMPRESSION_ASTC_4X4 = 3,
} MPCompression;

/** Receives one line of progress, a warning or an error. */
typedef void (*MPLogFunction)(const char* message, void* user_data);

#define MP_MAX_PATH 2048

/**
 * Where a source image comes from: a plain image file, or an image inside a .gltf/.glb.
 * A null or empty path means "no image".
 */
typedef struct MPImageSource
{
	/// An image file, or the .gltf/.glb holding the image
	const char* pcPath;

	/// If greater than zero, an index into the glTF's images. If less than zero,  path is an image file
	int GLTFImageIndex;

	/// Channel to read for single-component maps (0-3), or -1 for the whole image / luminance
	int Channel;
} MPImageSource;

typedef struct MPFallbackValues
{
	float Roughness;
	float Metallic;
	float Occlusion;
} MPFallbackValues;

typedef struct MPBakeSettings
{
	MPImageSource SourceDiffuse;
	MPImageSource SourceNormal;
	MPImageSource SourceRoughness;
	MPImageSource SourceMetallic;
	MPImageSource SourceOcclusion;

	/// Used for the ORM channels that have no source image
	MPFallbackValues FallbackValues;

	const char* pcOutputDir;

	/// Base name to be prepended when building a file name. For example, "{basename}_diffuse.ktx2"
	const char* pcBaseName;

	MPBool32 bCreateSubFolder;
	MPBool32 bGenerateMipmaps;

	/// 1 = full size, 2 = half, etc.
	int ResolutionDivisor;

	MPCompression Compression;
} MPBakeSettings;


typedef struct MPBakeOutputs
{
	char pPathDiffuse[MP_MAX_PATH];
	char pPathNormal[MP_MAX_PATH];
	char pPathORM[MP_MAX_PATH];
} MPBakeOutputs;

typedef struct MPExportOptions
{
	int bEmbedTexturesInGLTF;

	/// If true, links textures with KHR_texture_basisu, as the GLTF spec requires. BasisU is not supported by all
	/// loaders.
	int bUseBasisUExtension;
} MPExportOptions;

/**
 * @brief The baked textures to link into one GLTF material. A null or empty path leaves that slot as it was.
 */
typedef struct MPMaterialLink
{
	int MaterialIndex;

	const char* pcBaseColor;
	const char* pcNormal;
	const char* pcMetallicRoughness;
	const char* pcOcclusion;
} MPMaterialLink;

typedef struct MPTextureSlot
{
	MPImageSource Source;

	/// Width, zero if the image could not be read.
	int Width;
	int Height;
} MPTextureSlot;

typedef struct MPGltfMaterial
{
	const char* pcName;

	/// The index into the GLTF's materials list
	int MaterialIndex;

	MPTextureSlot BaseColor;
	MPTextureSlot Normal;

	MPTextureSlot Occlusion;
	MPTextureSlot Roughness;
	MPTextureSlot Metallic;
} MPGltfMaterial;

/** The materials of a glTF model. Created by MPGLTFMaterialsLoad, released by MPGLTFMaterialsFree. */
typedef struct MPGltfMaterials MPGltfMaterials;

/**
 * @brief Returns the compiled API version. Check against `MP_API_VERSION`
 */
MP_API int MPGetAPIVersion(void) MP_NOEXCEPT;

/**
 * @brief Returns the last error or an empty string if there was none
 */
MP_API const char* MPLastError(void) MP_NOEXCEPT;

/**
 * @brief Sets bake settings to their defaults
 */
MP_API void MPInitBakeSettings(MPBakeSettings* settings) MP_NOEXCEPT;

/**
 * @brief Sets export settings to their defaults
 */
MP_API void MPInitExportSettings(MPExportOptions* options) MP_NOEXCEPT;

MP_API MPResult MPBake(const MPBakeSettings* settings, MPBakeOutputs* outputs, MPLogFunction log,
					   void* user_data) MP_NOEXCEPT;

MP_API MPResult MPGLTFMaterialsLoad(const char* path, MPGltfMaterials** materials) MP_NOEXCEPT;
MP_API int MPGLTFMaterialsCount(const MPGltfMaterials* materials) MP_NOEXCEPT;
MP_API MPResult MPGLTFMaterialsGet(const MPGltfMaterials* materials, int index, MPGltfMaterial* material) MP_NOEXCEPT;
MP_API void MPGLTFMaterialsFree(MPGltfMaterials* materials) MP_NOEXCEPT;

MP_API MPResult MPGLTFExport(const char* source_path, const char* output_path, const MPMaterialLink* links,
							 int link_count, const MPExportOptions* options, MPLogFunction log,
							 void* user_data) MP_NOEXCEPT;

MP_API MPResult MPGLTFBake(const char* source_path, const char* output_path, const MPBakeSettings* settings,
						   const MPExportOptions* options, MPLogFunction log, void* user_data) MP_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif /* MATPACKER_H */
