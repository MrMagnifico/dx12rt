//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#ifndef RAYTRACING_HLSL
#define RAYTRACING_HLSL

#include "hlsl/RaytracingHlslCompat.h"
#include "Materials.hlsl"

RaytracingAccelerationStructure Scene : register(t0, space0);
RWTexture2D<float4> RenderTarget : register(u0);
StructuredBuffer<PointLight> PointLights : register(t1, space0);
StructuredBuffer<MaterialPBR> Materials : register(t2, space0);
ByteAddressBuffer MaterialIndices: register(t3, space0);
ByteAddressBuffer Indices : register(t4, space0);
StructuredBuffer<Vertex> Vertices : register(t5, space0);

ConstantBuffer<SceneConstantBuffer> g_sceneCB : register(b0);
ConstantBuffer<MaterialConstantBuffer> g_materialCB : register(b1);

struct RayPayload {
    // Input
    bool isShadowRay;
    
    // Output
    float3 color;
    bool hit;
};

// Retrieve hit world position.
float3 HitWorldPosition() {
    return WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
}

// Retrieve attribute at a hit position interpolated from vertex attributes using the hit's barycentrics.
float3 HitAttribute(float3 vertexAttribute[3], BuiltInTriangleIntersectionAttributes attr) {
    return vertexAttribute[0] +
        attr.barycentrics.x * (vertexAttribute[1] - vertexAttribute[0]) +
        attr.barycentrics.y * (vertexAttribute[2] - vertexAttribute[0]);
}

// Generate a ray in world space for a camera pixel corresponding to an index from the dispatched 2D grid.
inline void GenerateCameraRay(uint2 index, out float3 origin, out float3 direction) {
    float2 xy = index + 0.5f; // center in the middle of the pixel.
    float2 screenPos = xy / DispatchRaysDimensions().xy * 2.0 - 1.0;

    // Invert Y for DirectX-style coordinates.
    screenPos.y = -screenPos.y;

    // Unproject the pixel coordinate into a ray.
    float4 world = mul(float4(screenPos, 0, 1), g_sceneCB.projectionToWorld);

    world.xyz /= world.w;
    origin = g_sceneCB.cameraPosition.xyz;
    direction = normalize(world.xyz - origin);
}

float3 LightingPBR(float3 hitPosition, float3 cameraDirection, float3 normal,
                   MaterialPBR material, float3 F0,
                   float3 lightSamplePosition, float3 lightSampleColor) {
    // Radiance and geometry terms
    float3 L            = normalize(lightSamplePosition - hitPosition);
    float3 H            = normalize(cameraDirection + L);
    float distance      = length(lightSamplePosition - hitPosition);
    float attenuation   = 1.0f / (distance * distance);
    float3 radiance     = lightSampleColor * attenuation;
        
    // Cook-Torrence BRDF
    float NDF   = DistributionGGX(normal, H, material.roughness);
    float G     = GeometrySmith(normal, cameraDirection, L, material.roughness);
    float3 F    = fresnelSchlick(max(dot(H, cameraDirection), 0.0f), F0);
    
    // Diffuse
    float3 kS   = F;
    float3 kD   = float3(1.0f, 1.0f, 1.0f) - kS;
    kD          *= 1.0f - material.metallic;
        
    // Specular
    float3 numerator    = NDF * G * F;
    float denominator   = 4.0f * max(dot(normal, cameraDirection), 0.0f) * max(dot(normal, L), 0.0f) + 0.0001f;
    float3 specular     = numerator / denominator;
            
    // Compute outgoing irradiance
    float NdotL = max(dot(normal, L), 0.0);
    return (kD * material.albedo / PI + specular) * radiance * NdotL;
}

// Full lighting calculation.
float3 CalculateLighting(float3 hitPosition, float3 cameraDirection, float3 normal, MaterialPBR material) {
    // Constants given the material
    float3 F0   = float3(0.04f, 0.04f, 0.04f);
    F0          = lerp(F0, material.albedo, material.metallic);
    
    uint numLights, lightSize;
    PointLights.GetDimensions(numLights, lightSize);
    float3 accumulatedColor = float3(0.0f, 0.0f, 0.0f);
    for (uint i = 0u; i < numLights; i++) {
        PointLight pointLight   = PointLights[i];
        
        // Trace a shadow ray and skip this light's contribution if it's obscured
        RayDesc shadowRay;
        shadowRay.Origin    = hitPosition;
        shadowRay.Direction = pointLight.position - hitPosition;
        shadowRay.TMin      = 0.001f;
        shadowRay.TMax      = 1.0f;
        RayPayload shadowPayload;
        shadowPayload.isShadowRay = true;
        TraceRay(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, 0, 0, 0, shadowRay, shadowPayload);
        if (shadowPayload.hit) {
            continue;
        }
        
        // Compute contribution from this light
        accumulatedColor += LightingPBR(hitPosition, cameraDirection, normal, material, F0, pointLight.position, pointLight.color);
    }
    return accumulatedColor;
}

[shader("raygeneration")]
void MyRaygenShader() {
    // Generate a ray for a camera pixel corresponding to an index from the dispatched 2D grid.
    float3 rayDir;
    float3 origin;
    GenerateCameraRay(DispatchRaysIndex().xy, origin, rayDir);

    // Trace the ray.
    // Set the ray's extents.
    RayDesc ray;
    ray.Origin      = origin;
    ray.Direction   = rayDir;
    ray.TMin        = 0.001f;
    ray.TMax        = 10000.0f;
    RayPayload payload = { false, float3(0.0f, 0.0f, 0.0f), false };
    TraceRay(Scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, 0, 0, 0, ray, payload);

    // Write the raytraced color to the output texture.
    RenderTarget[DispatchRaysIndex().xy] = float4(payload.color, 1.0f); // Pixel is fully opaque
}

[shader("closesthit")]
void MyClosestHitShader(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    if (payload.isShadowRay) {
        payload.hit = true;
    } else {

        // Load up 3 32 bit indices for the triangle.
        static const uint indexSizeInBytes      = 4;
        static const uint indicesPerTriangle    = 3;
        static const uint triangleIndexStride   = indicesPerTriangle * indexSizeInBytes;
        const uint baseIndex                    = PrimitiveIndex() * triangleIndexStride;
        const uint3 indices                     = Indices.Load3(baseIndex);
        
        // Load the corrsponding material for the triangle (or the default material if this triangle does not have one).
        static const uint materialIndexSizeInBytes  = 4;
        const uint triangleIndex                    = PrimitiveIndex() * materialIndexSizeInBytes;
        const int materialIndex                     = asint(MaterialIndices.Load(triangleIndex));
        MaterialPBR triangleMaterial;
        if (materialIndex == -1) {
            // No material corresponding to this triangle, use default material properties
            triangleMaterial.albedo     = g_materialCB.defaultAlbedo.rgb;
            triangleMaterial.metallic   = g_materialCB.defaultMetalAndRoughness.r;
            triangleMaterial.roughness  = g_materialCB.defaultMetalAndRoughness.g;
        } else {
            triangleMaterial = Materials[materialIndex];
        }

        // Retrieve corresponding vertex normals for the triangle vertices.
        float3 vertexNormals[3] = { 
            Vertices[indices[0]].normal, 
            Vertices[indices[1]].normal, 
            Vertices[indices[2]].normal 
        };

        // Compute the triangle's normal.
        float3 triangleNormal = HitAttribute(vertexNormals, attr);

        // Compute the final pixel color
        float3 hitPosition      = HitWorldPosition();
        float3 cameraDirection  = -WorldRayDirection();
        float3 diffuseColor     = CalculateLighting(hitPosition, cameraDirection, triangleNormal, triangleMaterial);
        float3 pixelColor       = diffuseColor + float3(0.1f, 0.1f, 0.1f); // Add a constant ambient term
    
        // Populate payload members
        payload.hit     = true;
        payload.color   = pixelColor;
    }
}

[shader("miss")]
void MyMissShader(inout RayPayload payload)
{
    if (payload.isShadowRay) {
        payload.hit = false;
    } else {
        static const float3 background = float3(0.0f, 0.2f, 0.4f);
    
        payload.color   = background;
        payload.hit     = false;
    }
}

#endif // RAYTRACING_HLSL