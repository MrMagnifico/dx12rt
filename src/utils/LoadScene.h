#pragma once

#include "../RayTracingHlslCompat.h"
#include "stdafx.h"


namespace LoadScene {
struct LoadedObj {
	std::vector<Vertex> vertices;
	std::vector<Index> indices;
};

LoadedObj load_obj(std::string path);
}

