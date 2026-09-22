#pragma once

#include "ImageSource.hpp"

#include <functional>
#include <string>
#include <vector>


/**
 * @brief The order to pack components in
 */
enum class eORMPackOrder
{
	ORM, // AO, Roughness, Metallic
	OMR, // AO, Metallic, Roughness
};

struct BakeSettings
{
	ImageSource PathDiffuse;
	ImageSource PathNormalMap;
	ImageSource PathRoughness;
	ImageSource PathMetallic;
	ImageSource PathAO;

	float DefaultRoughness = 0.5f;
	float DefaultMetallic = 0.0f;
	float DefaultAO = 1.0f;

	std::string OutputDir;
	std::string BaseName;

	// Write the files into <OutputDir>/<BaseName>/ instead of directly into <OutputDir>.
	bool bCreateSubfolder = false;

	bool bExportMipmaps = true;

	// Output resolution is the source resolution divided by this (1 = full, 2 = half, ...).
	int ResolutionDivisor = 1;

	eTextureCompression Compression = eTextureCompression::None;
};

// Paths of the files a bake wrote; empty where that output was skipped or failed.
struct BakeOutputs
{
	std::string Diffuse;
	std::string Normal;
	std::string Orm;

	std::vector<std::string> All() const
	{
		std::vector<std::string> all;

		for (const std::string* path : { &Diffuse, &Normal, &Orm }) {
			if (!path->empty()) {
				all.push_back(*path);
			}
		}

		return all;
	}
};

// Bakes the configured textures to <outputDir>/<baseName>_{diffuse,normal,orm}.ktx2.
// Progress and errors are reported through `log`; paths of successfully written files are stored in `outputs` if
// given. Returns true if every requested output succeeded.
bool Bake(const BakeSettings& settings, const std::function<void(const std::string&)>& log,
		  BakeOutputs* outputs = nullptr);
