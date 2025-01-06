#include "stdafx.h"
#include "LoadScene.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "../tinyobjloader/tiny_obj_loader.h"

#include <iostream>
#include <map>


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

    // Loop over shapes. We group the faces by their material so we can create separate 'objects' out of them in the returned struct
    std::map<int, Indices> indices_by_material;
    std::map<int, Vertices> vertices_by_material;
    std::map<int, size_t> material_index_offset;
    for (size_t s = 0; s < shapes.size(); s++) {
        // Loop over faces
        size_t shape_index_offset = 0ULL;
        for (size_t f = 0; f < shapes[s].mesh.num_face_vertices.size(); f++) {
            int material_id = shapes[s].mesh.material_ids[f];
            if (!indices_by_material.contains(material_id) && !vertices_by_material.contains(material_id)) {
                indices_by_material[material_id]    = Indices();
                vertices_by_material[material_id]   = Vertices();
                material_index_offset[material_id]  = 0ULL;
            }

            // Loop over vertices in the face.
            size_t fv = size_t(shapes[s].mesh.num_face_vertices[f]);
            for (size_t v = 0; v < fv; v++) {
                // Get face index and ensure normals are present
                tinyobj::index_t idx = shapes[s].mesh.indices[shape_index_offset + v];
                assert(idx.normal_index >= 0);

                // Index data
                indices_by_material[material_id].push_back(static_cast<UINT32>(material_index_offset[material_id] + v));

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
                vertices_by_material[material_id].push_back(vertex);
            }
            shape_index_offset                  += fv;
            material_index_offset[material_id]  += fv;
        }
    }

    // Create separate 'objects' in return struct
    // TODO: Account for objects with material index -1 (i.e. should use the default material)
    for (const auto& [material_id, indices] : indices_by_material) {
        loaded_obj.indices_per_object.push_back(indices_by_material[material_id]);
        loaded_obj.vertices_per_object.push_back(vertices_by_material[material_id]);
    }

    // Loop over materials
    for (const tinyobj::material_t& material : materials) {
        MaterialPBR pbr = {};

        // Compute albedo as weighted average of diffuse and specular
        float albedo_normalizing_factor_r = material.diffuse[0] + material.specular[0];
        pbr.albedo.x = (material.diffuse[0] / albedo_normalizing_factor_r) * material.diffuse[0] + (material.specular[0] / albedo_normalizing_factor_r) * material.specular[0];
        float albedo_normalizing_factor_g = material.diffuse[1] + material.specular[1];
        pbr.albedo.y = (material.diffuse[1] / albedo_normalizing_factor_r) * material.diffuse[1] + (material.specular[1] / albedo_normalizing_factor_r) * material.specular[1];
        float albedo_normalizing_factor_b = material.diffuse[2] + material.specular[2];
        pbr.albedo.z = (material.diffuse[2] / albedo_normalizing_factor_r) * material.diffuse[2] + (material.specular[2] / albedo_normalizing_factor_r) * material.specular[2];

        // Compute roughness as a weighted average of specular components
        float specular_normalizing_factor = max(material.specular[0] + material.specular[1] + material.specular[2], 0.001f);
        float shininess = (material.specular[0] / specular_normalizing_factor) * material.specular[0] +
            (material.specular[1] / specular_normalizing_factor) * material.specular[1] +
            (material.specular[2] / specular_normalizing_factor) * material.specular[2];
        pbr.roughness = 1.0f - shininess;

        // Use a constant default metallic
        pbr.metallic = 0.25f;

        loaded_obj.materials.push_back(pbr);
    }

    return loaded_obj;
}
