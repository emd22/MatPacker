#pragma once

#include "ImageSource.hpp"

#include <functional>
#include <string>
#include <vector>

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

// Bakes the configured textures to <outputDir>/<baseName>_{diffuse,normal,orm}.ktx2.
// Progress and errors are reported through `log`; paths of successfully written files are appended to
// `written` if given. Returns true if every requested output succeeded.
bool Bake(const BakeSettings& settings, const std::function<void(const std::string&)>& log,
		  std::vector<std::string>* written = nullptr);
