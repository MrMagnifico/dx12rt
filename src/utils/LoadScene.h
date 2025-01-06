#pragma once

#include "../hlsl/RayTracingHlslCompat.h"
#include "stdafx.h"

using Indices			= std::vector<Index>;
using Vertices			= std::vector<Vertex>;
using MaterialIndices	= std::vector<Index>;

namespace LoadScene {
struct LoadedObj {
	// Geometry
	std::vector<Indices> indices_per_object;
	std::vector<Vertices> vertices_per_object;
	std::vector<MaterialIndices> material_indices_per_object; // Index into materials buffer on a per-triangle basis

	// Materials
	std::vector<MaterialPBR> materials;
};

LoadedObj load_obj(std::string path);
}

