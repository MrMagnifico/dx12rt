#pragma once

#include "../hlsl/RayTracingHlslCompat.h"
#include "stdafx.h"


namespace LoadScene {
struct LoadedObj {
	// Geometry
	std::vector<Vertex> vertices;
	std::vector<Index> indices;

	// Materials
	std::vector<int32_t> material_indices; // Index into material buffer per triangle
	std::vector<MaterialPBR> materials;
};

LoadedObj load_obj(std::string path);
}

