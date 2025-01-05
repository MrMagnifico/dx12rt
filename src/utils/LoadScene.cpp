#include "stdafx.h"
#include "LoadScene.h"

#include <iostream>
#define TINYOBJLOADER_IMPLEMENTATION
#include "../tinyobjloader/tiny_obj_loader.h"


LoadScene::LoadedObj LoadScene::load_obj(std::string path) {
    tinyobj::ObjReaderConfig reader_config;
    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(path, reader_config)) {
        if (!reader.Error().empty()) {
            std::cerr << "TinyObjReader: " << reader.Error();
        }
        exit(EXIT_FAILURE);
    }
    if (!reader.Warning().empty()) {
        std::cout << "TinyObjReader: " << reader.Warning();
    }

    LoadedObj loaded_obj = {};
    auto& attrib    = reader.GetAttrib();
    auto& shapes    = reader.GetShapes();
    auto& materials = reader.GetMaterials();

    // Loop over shapes
    for (size_t s = 0; s < shapes.size(); s++) {
        // Loop over faces(polygon)
        size_t index_offset = 0;
        for (size_t f = 0; f < shapes[s].mesh.num_face_vertices.size(); f++) {
            // Material index for this face
            loaded_obj.material_indices.push_back(shapes[s].mesh.material_ids[f]);

            // Loop over vertices in the face.
            size_t fv = size_t(shapes[s].mesh.num_face_vertices[f]);
            for (size_t v = 0; v < fv; v++) {
                // Get face index and ensure normals are present
                tinyobj::index_t idx = shapes[s].mesh.indices[index_offset + v];
                assert(idx.normal_index >= 0);

                // Index data
                loaded_obj.indices.push_back(static_cast<UINT32>(index_offset + v));

                // Process vertex data
                Vertex vertex = {};
                // Positions
                vertex.position.x   = attrib.vertices[3 * size_t(idx.vertex_index) + 0];
                vertex.position.y   = attrib.vertices[3 * size_t(idx.vertex_index) + 1];
                vertex.position.z   = attrib.vertices[3 * size_t(idx.vertex_index) + 2];
                // Normals
                vertex.normal.x = attrib.normals[3 * size_t(idx.normal_index) + 0];
                vertex.normal.y = attrib.normals[3 * size_t(idx.normal_index) + 1];
                vertex.normal.z = attrib.normals[3 * size_t(idx.normal_index) + 2];
                // Push vertex data to return object
                loaded_obj.vertices.push_back(vertex);
            }

            index_offset += fv;
        }
    }

    // Loop over materials
    for (const tinyobj::material_t& material : materials) {
        MaterialPBR pbr = {};
        
        // Compute albedo as weighted average of diffuse and specular
        float albedo_normalizing_factor_r   = material.diffuse[0] + material.specular[0];
        pbr.albedo.x                        = (material.diffuse[0] / albedo_normalizing_factor_r) * material.diffuse[0] + (material.specular[0] / albedo_normalizing_factor_r) * material.specular[0];
        float albedo_normalizing_factor_g   = material.diffuse[1] + material.specular[1];
        pbr.albedo.y                        = (material.diffuse[1] / albedo_normalizing_factor_r) * material.diffuse[1] + (material.specular[1] / albedo_normalizing_factor_r) * material.specular[1];
        float albedo_normalizing_factor_b   = material.diffuse[2] + material.specular[2];
        pbr.albedo.z                        = (material.diffuse[2] / albedo_normalizing_factor_r) * material.diffuse[2] + (material.specular[2] / albedo_normalizing_factor_r) * material.specular[2];

        // Compute roughness as a weighted average of specular components
        float specular_normalizing_factor   = max(material.specular[0] + material.specular[1] + material.specular[2], 0.001f);
        float shininess                     = (material.specular[0] / specular_normalizing_factor) * material.specular[0] +
                                              (material.specular[1] / specular_normalizing_factor) * material.specular[1] + 
                                              (material.specular[2] / specular_normalizing_factor) * material.specular[2];
        pbr.roughness = 1.0f - shininess;

        // Use a constant default metallic
        pbr.metallic = 0.25f;

        loaded_obj.materials.push_back(pbr);
    }

    return loaded_obj;
}
