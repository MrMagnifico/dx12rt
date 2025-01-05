#pragma once

#include "../hlsl/RayTracingHlslCompat.h"
#include "stdafx.h"

using Indices	= std::vector<Index>;
using Vertices	= std::vector<Vertex>;

namespace LoadScene {
struct LoadedObj {
	// Geometry
	std::vector<Indices> indices_per_object;
	std::vector<Vertices> vertices_per_object;

	// Materials
	std::vector<MaterialPBR> materials;
};

LoadedObj load_obj(std::string path);
}

