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

#ifndef RAYTRACINGHLSLCOMPAT_H
#define RAYTRACINGHLSLCOMPAT_H

#if defined(HLSL)
#include "HlslCompat.h"
#else
using namespace DirectX;

// Shader will use byte encoding to access indices.
typedef UINT32 Index;
typedef INT32 MaterialIndex;
#endif

struct SceneConstantBuffer
{
    // Camera
    XMMATRIX projectionToWorld;
    XMVECTOR cameraPosition;

    // Default material
    XMFLOAT4 defaultAlbedo;             // Alpha channel is not used
    XMFLOAT4 defaultMetalAndRoughness;  // R channel encodes metal, G channel encodes roughness, rest is unused
};

struct Vertex
{
    XMFLOAT3 position;
    XMFLOAT3 normal;
};

struct PointLight
{
    XMFLOAT3 position;
    XMFLOAT3 color;
};

struct MaterialPBR
{
    XMFLOAT3 albedo;
    float metallic;
    float roughness;
};

#endif // RAYTRACINGHLSLCOMPAT_H