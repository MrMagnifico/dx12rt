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

#include "utils/stdafx.h"
#include <filesystem>

#include "D3D12RaytracingSimpleLighting.h"
#include "DirectXRaytracingHelper.h"
#include "CompiledShaders\Raytracing.hlsl.h"

using namespace std;
using namespace DX;

const wchar_t* D3D12RaytracingSimpleLighting::c_hitGroupName = L"MyHitGroup";
const wchar_t* D3D12RaytracingSimpleLighting::c_raygenShaderName = L"MyRaygenShader";
const wchar_t* D3D12RaytracingSimpleLighting::c_closestHitShaderName = L"MyClosestHitShader";
const wchar_t* D3D12RaytracingSimpleLighting::c_missShaderName = L"MyMissShader";

D3D12RaytracingSimpleLighting::D3D12RaytracingSimpleLighting(UINT width, UINT height, std::wstring name) :
    DXSample(width, height, name),
    m_curRotationAngleRad(0.0f)
{
    UpdateForSizeChange(width, height);
}

void D3D12RaytracingSimpleLighting::OnInit()
{
    m_deviceResources = std::make_unique<DeviceResources>(
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_UNKNOWN,
        FrameCount,
        D3D_FEATURE_LEVEL_11_0,
        // Sample shows handling of use cases with tearing support, which is OS dependent and has been supported since TH2.
        // Since the sample requires build 1809 (RS5) or higher, we don't need to handle non-tearing cases.
        DeviceResources::c_RequireTearingSupport,
        m_adapterIDoverride
        );
    m_deviceResources->RegisterDeviceNotify(this);
    m_deviceResources->SetWindow(Win32Application::GetHwnd(), m_width, m_height);
    m_deviceResources->InitializeDXGIAdapter();

    ThrowIfFalse(IsDirectXRaytracingSupported(m_deviceResources->GetAdapter()),
        L"ERROR: DirectX Raytracing is not supported by your OS, GPU and/or driver.\n\n");

    m_deviceResources->CreateDeviceResources();
    m_deviceResources->CreateWindowSizeDependentResources();

    InitializeScene();

    CreateDeviceDependentResources();
    CreateWindowSizeDependentResources();
}

// Update camera matrices passed into the shader.
void D3D12RaytracingSimpleLighting::UpdateCameraMatrices()
{
    auto frameIndex = m_deviceResources->GetCurrentFrameIndex();

    m_sceneCB[frameIndex].cameraPosition = m_eye;
    float fovAngleY = 45.0f;
    XMMATRIX view = XMMatrixLookAtLH(m_eye, m_at, m_up);
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(fovAngleY), m_aspectRatio, 1.0f, 125.0f);
    XMMATRIX viewProj = view * proj;

    m_sceneCB[frameIndex].projectionToWorld = XMMatrixInverse(nullptr, viewProj);
}

// Initialize scene rendering parameters.
void D3D12RaytracingSimpleLighting::InitializeScene()
{
    auto frameIndex = m_deviceResources->GetCurrentFrameIndex();

    // Setup materials.
    // TODO: Get these from a GUI and update them every frame
    {
        UINT frameIndex                                 = m_deviceResources->GetCurrentFrameIndex();
        m_sceneCB[frameIndex].defaultAlbedo             = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        m_sceneCB[frameIndex].defaultMetalAndRoughness  = XMFLOAT4(0.1f, 0.8f, 0.0f, 0.0f);
    }

    // Setup camera.
    {
        // Initialize the view and projection inverse matrices.
        m_eye = { 0.0f, 1.5f, -4.0f, 1.0f };
        m_at = { 0.0f, 0.8f, 0.0f, 1.0f };
        XMVECTOR right = { 1.0f, 0.0f, 0.0f, 0.0f };

        XMVECTOR direction = XMVector4Normalize(m_at - m_eye);
        m_up = XMVector3Normalize(XMVector3Cross(direction, right));

        // Rotate camera around Y axis.
        XMMATRIX rotate = XMMatrixRotationY(XMConvertToRadians(45.0f));
        m_eye = XMVector3Transform(m_eye, rotate);
        m_up = XMVector3Transform(m_up, rotate);
        
        UpdateCameraMatrices();
    }

    // Apply the initial values to all frames' buffer instances.
    for (auto& sceneCB : m_sceneCB)
    {
        sceneCB = m_sceneCB[frameIndex];
    }
}

// Create constant buffers.
void D3D12RaytracingSimpleLighting::CreateConstantBuffers()
{
    ID3D12Device* device            = m_deviceResources->GetD3DDevice();
    D3D12MA::Allocator* allocator   = m_deviceResources->GetD3DMAllocator();
    UINT frameCount                 = m_deviceResources->GetBackBufferCount();
    
    // Allocate one constant buffer per frame, since it gets updated every frame
    D3D12MA::ALLOCATION_DESC allocationDesc             = {};
    allocationDesc.HeapType                             = D3D12_HEAP_TYPE_UPLOAD;
    const size_t cbSize                                 = frameCount * sizeof(AlignedSceneConstantBuffer);
    const D3D12_RESOURCE_DESC constantBufferDesc        = CD3DX12_RESOURCE_DESC::Buffer(cbSize);
    ThrowIfFailed(allocator->CreateResource(
        &allocationDesc,
        &constantBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        &m_perFrameConstants.allocation,
        IID_PPV_ARGS(&m_perFrameConstants.resource)));

    // Map the constant buffer and cache its heap pointers.
    // We don't unmap this until the app closes. Keeping buffer mapped for the lifetime of the resource is okay.
    CD3DX12_RANGE readRange(0, 0);        // We do not intend to read from this resource on the CPU.
    ThrowIfFailed(m_perFrameConstants.resource->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedConstantData)));
}

// Create resources that depend on the device.
void D3D12RaytracingSimpleLighting::CreateDeviceDependentResources()
{
    // Initialize raytracing pipeline.

    // Create raytracing interfaces: raytracing device and commandlist.
    CreateRaytracingInterfaces();

    // Create root signatures for the shaders.
    CreateRootSignatures();

    // Create a raytracing pipeline state object which defines the binding of shaders, state and resources to be used during raytracing.
    CreateRaytracingPipelineStateObject();

    // Create a heap for descriptors.
    CreateDescriptorHeap();

    // Build light sources buffers to be used for lighting
    BuildLightBuffers();

    // Build geometry and materials to be used.
    std::filesystem::path cornell_path  = "C:\\Users\\willy\\Documents\\Random Bullshit\\dx12-rt\\scenes\\obj\\CornellBox-Mirror-Rotated.obj";
    LoadScene::LoadedObj loaded_obj     = LoadScene::load_obj(cornell_path.string());
    BuildMaterials(loaded_obj);
    BuildGeometry(loaded_obj);

    // Build raytracing acceleration structures from the generated geometry.
    BuildAccelerationStructures();

    // Create constant buffers for the geometry and the scene.
    CreateConstantBuffers();

    // Build shader tables, which define shaders and their local root arguments.
    BuildShaderTables();

    // Create an output 2D texture to store the raytracing result to.
    CreateRaytracingOutputResource();
}

void D3D12RaytracingSimpleLighting::SerializeAndCreateVersionedRootSignature(D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc, ComPtr<ID3D12RootSignature>* rootSig)
{
    auto device = m_deviceResources->GetD3DDevice();
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;

    ThrowIfFailed(D3D12SerializeVersionedRootSignature(&desc, &blob, &error), error ? static_cast<wchar_t*>(error->GetBufferPointer()) : nullptr);
    ThrowIfFailed(device->CreateRootSignature(1, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&(*rootSig))));
}

void D3D12RaytracingSimpleLighting::CreateRootSignatures()
{
    auto device = m_deviceResources->GetD3DDevice();

    // Global Root Signature
    // This is a root signature that is shared across all raytracing shaders invoked during a DispatchRays() call.
    {
        CD3DX12_ROOT_PARAMETER rootParameters[BoundResourceSlots::BoundResourceSlotsCount];
        rootParameters[BoundResourceSlots::TLAS].InitAsShaderResourceView(0);
        rootParameters[BoundResourceSlots::SceneCB].InitAsConstantBufferView(0);
        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC globalRootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters, 0U, nullptr, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);
        SerializeAndCreateVersionedRootSignature(globalRootSignatureDesc, &m_raytracingGlobalRootSignature);
    }
}

// Create raytracing device and command list.
void D3D12RaytracingSimpleLighting::CreateRaytracingInterfaces()
{
    auto device = m_deviceResources->GetD3DDevice();
    auto commandList = m_deviceResources->GetCommandList();

    ThrowIfFailed(device->QueryInterface(IID_PPV_ARGS(&m_dxrDevice)), L"Couldn't get DirectX Raytracing interface for the device.\n");
    ThrowIfFailed(commandList->QueryInterface(IID_PPV_ARGS(&m_dxrCommandList)), L"Couldn't get DirectX Raytracing interface for the command list.\n");
}

// Create a raytracing pipeline state object (RTPSO).
// An RTPSO represents a full set of shaders reachable by a DispatchRays() call,
// with all configuration options resolved, such as local signatures and other state.
void D3D12RaytracingSimpleLighting::CreateRaytracingPipelineStateObject()
{
    // Create 7 subobjects that combine into a RTPSO:
    // Subobjects need to be associated with DXIL exports (i.e. shaders) either by way of default or explicit associations.
    // Default association applies to every exported shader entrypoint that doesn't have any of the same type of subobject associated with it.
    // This simple sample utilizes default shader association except for local root signature subobject
    // which has an explicit association specified purely for demonstration purposes.
    // 1 - DXIL library
    // 1 - Triangle hit group
    // 1 - Shader config
    // 2 - Local root signature and association
    // 1 - Global root signature
    // 1 - Pipeline config
    CD3DX12_STATE_OBJECT_DESC raytracingPipeline{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE };


    // DXIL library
    // This contains the shaders and their entrypoints for the state object.
    // Since shaders are not considered a subobject, they need to be passed in via DXIL library subobjects.
    auto lib = raytracingPipeline.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
    D3D12_SHADER_BYTECODE libdxil = CD3DX12_SHADER_BYTECODE((void *)g_pRaytracing, ARRAYSIZE(g_pRaytracing));
    lib->SetDXILLibrary(&libdxil);
    // Define which shader exports to surface from the library.
    // If no shader exports are defined for a DXIL library subobject, all shaders will be surfaced.
    // In this sample, this could be ommited for convenience since the sample uses all shaders in the library. 
    {
        lib->DefineExport(c_raygenShaderName);
        lib->DefineExport(c_closestHitShaderName);
        lib->DefineExport(c_missShaderName);
    }
    
    // Triangle hit group
    // A hit group specifies closest hit, any hit and intersection shaders to be executed when a ray intersects the geometry's triangle/AABB.
    // In this sample, we only use triangle geometry with a closest hit shader, so others are not set.
    auto hitGroup = raytracingPipeline.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
    hitGroup->SetClosestHitShaderImport(c_closestHitShaderName);
    hitGroup->SetHitGroupExport(c_hitGroupName);
    hitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);
    
    // Shader config
    // Defines the maximum sizes in bytes for the ray payload and attribute structure.
    auto shaderConfig   = raytracingPipeline.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
    UINT payloadSize    = 20;               // size of RayPayload
    UINT attributeSize  = sizeof(XMFLOAT2); // float2 barycentrics
    shaderConfig->Config(payloadSize, attributeSize);

    // Global root signature
    // This is a root signature that is shared across all raytracing shaders invoked during a DispatchRays() call.
    auto globalRootSignature = raytracingPipeline.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    globalRootSignature->SetRootSignature(m_raytracingGlobalRootSignature.Get());

    // Pipeline config
    // Defines the maximum TraceRay() recursion depth.
    auto pipelineConfig = raytracingPipeline.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    // PERFOMANCE TIP: Set max recursion depth as low as needed 
    // as drivers may apply optimization strategies for low recursion depths.
    UINT maxRecursionDepth = 2; // ~ primary and shadow rays only. 
    pipelineConfig->Config(maxRecursionDepth);

#if _DEBUG
    PrintStateObjectDesc(raytracingPipeline);
#endif

    // Create the state object.
    ThrowIfFailed(m_dxrDevice->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_dxrStateObject)), L"Couldn't create DirectX Raytracing state object.\n");
}

// Create 2D output texture for raytracing.
void D3D12RaytracingSimpleLighting::CreateRaytracingOutputResource()
{
    ID3D12Device* device            = m_deviceResources->GetD3DDevice();
    D3D12MA::Allocator* allocator   = m_deviceResources->GetD3DMAllocator();
    DXGI_FORMAT backbufferFormat    = m_deviceResources->GetBackBufferFormat();

    // Create the output resource. The dimensions and format should match the swap-chain.
    CD3DX12_RESOURCE_DESC uavDesc           = CD3DX12_RESOURCE_DESC::Tex2D(backbufferFormat, m_width, m_height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12MA::ALLOCATION_DESC allocationDesc = {};
    allocationDesc.HeapType                 = D3D12_HEAP_TYPE_DEFAULT;
    ThrowIfFailed(allocator->CreateResource(&allocationDesc, &uavDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, &m_raytracingOutput.allocation, IID_PPV_ARGS(&m_raytracingOutput.resource)));
    NAME_D3D12_OBJECT(m_raytracingOutput.resource);

    D3D12_CPU_DESCRIPTOR_HANDLE uavDescriptorHandle;
    AllocateDescriptor(&uavDescriptorHandle, DescriptorHeapSlots::OutputRenderTarget);
    D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc    = {};
    UAVDesc.ViewDimension                       = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(m_raytracingOutput.resource.Get(), nullptr, &UAVDesc, uavDescriptorHandle);
    m_raytracingOutputResourceUAVGpuDescriptor = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), DescriptorHeapSlots::OutputRenderTarget, m_descriptorSize);
}

void D3D12RaytracingSimpleLighting::CreateDescriptorHeap()
{
    auto device = m_deviceResources->GetD3DDevice();

    D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc = {};
    descriptorHeapDesc.NumDescriptors = 200U; // Should be enough for all the descriptors we could possibly need
    descriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    descriptorHeapDesc.NodeMask = 0;
    device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&m_descriptorHeap));
    NAME_D3D12_OBJECT(m_descriptorHeap);

    m_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

// Build geometry used in the sample.
void D3D12RaytracingSimpleLighting::BuildGeometry(LoadScene::LoadedObj loaded_obj)
{
    D3D12MA::Allocator* allocator               = m_deviceResources->GetD3DMAllocator();
    ID3D12GraphicsCommandList* commandList      = m_deviceResources->GetCommandList();
    ID3D12CommandAllocator* commandAllocator    = m_deviceResources->GetCommandAllocator();
    const size_t num_objects                    = loaded_obj.indices_per_object.size();

    // Reset the command list so we can issue copy command and resource transitions for staging buffer copies
    commandList->Reset(commandAllocator, nullptr);

    // These staging buffers will be automatically free'd when these vectors go out of scope after the method is done executing
    std::vector<D3DBuffer> indexStagingBuffers(num_objects);
    std::vector<D3DBuffer> vertexStagingBuffers(num_objects);
    std::vector<D3DBuffer> materialIndexStagingBuffers(num_objects);

    m_indexBuffers.resize(num_objects);
    m_vertexBuffers.resize(num_objects);
    m_materialIndexBuffers.resize(num_objects);
    for (size_t i = 0ULL; i < num_objects; i++) {
        // Retrieve raw data
        Indices& object_indices             = loaded_obj.indices_per_object[i];
        Vertices& object_vertices           = loaded_obj.vertices_per_object[i];
        Indices& object_material_indices    = loaded_obj.material_indices_per_object[i];

        // Create staging and device-side buffers
        size_t indicesSize          = object_indices.size() * sizeof(Index);
        size_t verticesSize         = object_vertices.size() * sizeof(Vertex);
        size_t materialIndicesSize  = object_material_indices.size() * sizeof(Index);
        AllocateUploadBuffer(allocator, object_indices.data(), indicesSize, &indexStagingBuffers[i].resource.resource, &indexStagingBuffers[i].resource.allocation, L"IndicesStaging");
        AllocateUploadBuffer(allocator, object_vertices.data(), verticesSize, &vertexStagingBuffers[i].resource.resource, &vertexStagingBuffers[i].resource.allocation, L"VerticesStaging");
        AllocateUploadBuffer(allocator, object_material_indices.data(), materialIndicesSize, &materialIndexStagingBuffers[i].resource.resource, &materialIndexStagingBuffers[i].resource.allocation, L"MaterialIndicesStaging");
        AllocateDeviceBuffer(allocator, indicesSize, &m_indexBuffers[i].resource.resource, &m_indexBuffers[i].resource.allocation, false, D3D12_RESOURCE_STATE_COPY_DEST, L"Indices");
        AllocateDeviceBuffer(allocator, verticesSize, &m_vertexBuffers[i].resource.resource, &m_vertexBuffers[i].resource.allocation, false, D3D12_RESOURCE_STATE_COPY_DEST, L"Vertices");
        AllocateDeviceBuffer(allocator, materialIndicesSize, &m_materialIndexBuffers[i].resource.resource, &m_materialIndexBuffers[i].resource.allocation, false, D3D12_RESOURCE_STATE_COPY_DEST, L"MaterialIndicess");

        // Create SRVs for device-side buffers
        UINT object_srv_idx_base = DescriptorHeapSlots::IndexVertexMaterialBuffersBegin + (static_cast<UINT>(i) * 3U);
        CreateBufferSRV(&m_indexBuffers[i], static_cast<UINT>(object_indices.size()), 0, object_srv_idx_base);
        CreateBufferSRV(&m_vertexBuffers[i], static_cast<UINT>(object_vertices.size()), sizeof(Vertex), object_srv_idx_base + 1U);
        CreateBufferSRV(&m_materialIndexBuffers[i], static_cast<UINT>(object_material_indices.size()), 0, object_srv_idx_base + 2U);

        // Queue copies from staging buffer copies and transitions to SRV state
        commandList->CopyResource(m_indexBuffers[i].resource.resource.Get(), indexStagingBuffers[i].resource.resource.Get());
        commandList->CopyResource(m_vertexBuffers[i].resource.resource.Get(), vertexStagingBuffers[i].resource.resource.Get());
        commandList->CopyResource(m_materialIndexBuffers[i].resource.resource.Get(), materialIndexStagingBuffers[i].resource.resource.Get());
        CD3DX12_RESOURCE_BARRIER srvTransitions[3] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_indexBuffers[i].resource.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_vertexBuffers[i].resource.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_materialIndexBuffers[i].resource.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        };
        commandList->ResourceBarrier(3, srvTransitions);
    }

    // Kick off staging buffers copy and wait for GPU to finish as the locally created temporary GPU resources will get released once we go out of scope
    m_deviceResources->ExecuteCommandList();
    m_deviceResources->WaitForGpu();
}

void D3D12RaytracingSimpleLighting::BuildMaterials(LoadScene::LoadedObj loaded_obj)
{
    D3D12MA::Allocator* allocator               = m_deviceResources->GetD3DMAllocator();
    ID3D12GraphicsCommandList* commandList      = m_deviceResources->GetCommandList();
    ID3D12CommandAllocator* commandAllocator    = m_deviceResources->GetCommandAllocator();

    // Reset the command list so we can issue copy command and resource transitions for staging buffer copies
    commandList->Reset(commandAllocator, nullptr);

    // Create device buffer, staging buffer, and an SRV for the device buffer
    D3DBuffer materialsStagingBuffer;
    size_t materialsSize = loaded_obj.materials.size() * sizeof(MaterialPBR);
    AllocateUploadBuffer(allocator, loaded_obj.materials.data(), materialsSize, &materialsStagingBuffer.resource.resource, &materialsStagingBuffer.resource.allocation, L"MaterialsStaging");
    AllocateDeviceBuffer(allocator, materialsSize, &m_materialsBuffer.resource.resource, &m_materialsBuffer.resource.allocation, false, D3D12_RESOURCE_STATE_COPY_DEST, L"Materials");
    CreateBufferSRV(&m_materialsBuffer, static_cast<UINT>(loaded_obj.materials.size()), sizeof(MaterialPBR), DescriptorHeapSlots::MaterialsBuffer);

    // Queue copies from staging buffer copies and transitions to SRV state
    commandList->CopyResource(m_materialsBuffer.resource.resource.Get(), materialsStagingBuffer.resource.resource.Get());
    CD3DX12_RESOURCE_BARRIER srvTransition = CD3DX12_RESOURCE_BARRIER::Transition(m_materialsBuffer.resource.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &srvTransition);

    // Kick off staging buffer copy and wait for GPU to finish as the locally created temporary GPU resources will get released once we go out of scope
    m_deviceResources->ExecuteCommandList();
    m_deviceResources->WaitForGpu();
}

// Build acceleration structures needed for raytracing.
void D3D12RaytracingSimpleLighting::BuildAccelerationStructures()
{
    D3D12MA::Allocator* allocator               = m_deviceResources->GetD3DMAllocator();
    ID3D12GraphicsCommandList* commandList      = m_deviceResources->GetCommandList();
    ID3D12CommandAllocator* commandAllocator    = m_deviceResources->GetCommandAllocator();
    const size_t num_objects                    = m_indexBuffers.size();

    // Reset the command list for the acceleration structure construction.
    commandList->Reset(commandAllocator, nullptr);

    // Create per-object BLAS geometry descriptions
    D3D12_RAYTRACING_GEOMETRY_DESC baseGeometryDesc         = {};
    baseGeometryDesc.Type                                   = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    baseGeometryDesc.Triangles.IndexFormat                  = DXGI_FORMAT_R32_UINT;
    baseGeometryDesc.Triangles.Transform3x4                 = 0;
    baseGeometryDesc.Triangles.VertexFormat                 = DXGI_FORMAT_R32G32B32_FLOAT;
    baseGeometryDesc.Triangles.VertexBuffer.StrideInBytes   = sizeof(Vertex);
    baseGeometryDesc.Flags                                  = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE; // TODO: Change this if we ever decide to support transparent geometry
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> blasDescs(num_objects, baseGeometryDesc);
    for (size_t i = 0ULL; i < num_objects; i++) {
        blasDescs[i].Triangles.VertexBuffer.StartAddress    = m_vertexBuffers[i].resource.resource->GetGPUVirtualAddress();
        blasDescs[i].Triangles.VertexCount                  = static_cast<UINT>(m_vertexBuffers[i].resource.resource->GetDesc().Width) / sizeof(Vertex);
        blasDescs[i].Triangles.IndexBuffer                  = m_indexBuffers[i].resource.resource->GetGPUVirtualAddress();
        blasDescs[i].Triangles.IndexCount                   = static_cast<UINT>(m_indexBuffers[i].resource.resource->GetDesc().Width) / sizeof(Index);
    }

    // For both BLASes and TLASes, we would like a slow build in exchange for fast tracing
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    
    // Get prebuild info for the BLASes
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC baseBlasBuildDesc    = {};
    baseBlasBuildDesc.Inputs.DescsLayout                                    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    baseBlasBuildDesc.Inputs.Flags                                          = buildFlags;
    baseBlasBuildDesc.Inputs.NumDescs                                       = 1;
    baseBlasBuildDesc.Inputs.Type                                           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    std::vector<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO> blasPrebuildInfos(num_objects);
    std::vector<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC> blasBuildDescs(num_objects, baseBlasBuildDesc);
    for (size_t i = 0ULL; i < blasPrebuildInfos.size(); i++) {
        blasBuildDescs[i].Inputs.pGeometryDescs = &blasDescs[i];
        m_dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&blasBuildDescs[i].Inputs, &blasPrebuildInfos[i]);
        ThrowIfFalse(blasPrebuildInfos[i].ResultDataMaxSizeInBytes > 0);
    }
    
    // Get prebuild info for the TLAS
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasBuildDesc    = {};
    tlasBuildDesc.Inputs.DescsLayout                                    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlasBuildDesc.Inputs.Flags                                          = buildFlags;
    tlasBuildDesc.Inputs.NumDescs                                       = static_cast<UINT>(num_objects);
    tlasBuildDesc.Inputs.pGeometryDescs                                 = nullptr;
    tlasBuildDesc.Inputs.Type                                           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topLevelPrebuildInfo = {};
    m_dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&tlasBuildDesc.Inputs, &topLevelPrebuildInfo);
    ThrowIfFalse(topLevelPrebuildInfo.ResultDataMaxSizeInBytes > 0);

    // Allocate scratch space for BLAS builds
    std::vector<D3DResource> scratchResourcesBlas(num_objects);
    for (size_t i = 0ULL; i < num_objects; i++) {
        AllocateDeviceBuffer(allocator, blasPrebuildInfos[i].ScratchDataSizeInBytes, &scratchResourcesBlas[i].resource, &scratchResourcesBlas[i].allocation, true);
    }
    
    // Allocate scratch space for TLAS build
    D3DResource scratchResourceTlas;
    AllocateDeviceBuffer(allocator, topLevelPrebuildInfo.ScratchDataSizeInBytes, &scratchResourceTlas.resource, &scratchResourceTlas.allocation, true);

    // Acceleration structures can only be placed in resources that are created in the default heap (or custom heap equivalent). 
    // The resources that will contain acceleration structures must be created in the state D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, 
    // and must have resource flag D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS. The ALLOW_UNORDERED_ACCESS requirement simply acknowledges both: 
    //  - the system will be doing this type of access in its implementation of acceleration structure builds behind the scenes.
    //  - from the app point of view, synchronization of writes/reads to acceleration structures is accomplished using UAV barriers.
    D3D12_RESOURCE_STATES initialResourceState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
        
    // Allocate buffers for the actual BLASes and TLAS
    m_bottomLevelAccelerationStructures.resize(num_objects);
    for (size_t i = 0ULL; i < num_objects; i++) {
        AllocateDeviceBuffer(allocator, blasPrebuildInfos[i].ResultDataMaxSizeInBytes, &m_bottomLevelAccelerationStructures[i].resource, &m_bottomLevelAccelerationStructures[i].allocation, true, initialResourceState);
    }
    AllocateDeviceBuffer(allocator, topLevelPrebuildInfo.ResultDataMaxSizeInBytes, &m_topLevelAccelerationStructure.resource, &m_topLevelAccelerationStructure.allocation, true, initialResourceState);
    
    // Create an instance for each BLAS
    D3D12_RAYTRACING_INSTANCE_DESC baseInstanceDesc = {};
    baseInstanceDesc.Transform[0][0] = baseInstanceDesc.Transform[1][1] = baseInstanceDesc.Transform[2][2] = 1;
    baseInstanceDesc.InstanceMask                   = 1;
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs(num_objects, baseInstanceDesc);
    for (size_t i = 0ULL; i < num_objects; i++) {
        instanceDescs[i].AccelerationStructure  = m_bottomLevelAccelerationStructures[i].resource->GetGPUVirtualAddress();
        instanceDescs[i].InstanceID             = static_cast<UINT>(i); // This value will be used to reference this instance in HLSL shader code
    }
    D3DResource blasInstanceDescsBuffer;
    AllocateUploadBuffer(allocator, instanceDescs.data(), instanceDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), &blasInstanceDescsBuffer.resource, &blasInstanceDescsBuffer.allocation, L"InstanceDescs");

    // Update BLAS build descriptions with GPU-allocated resources
    for (size_t i = 0ULL; i < num_objects; i++) {
        blasBuildDescs[i].ScratchAccelerationStructureData  = scratchResourcesBlas[i].resource->GetGPUVirtualAddress();
        blasBuildDescs[i].DestAccelerationStructureData     = m_bottomLevelAccelerationStructures[i].resource->GetGPUVirtualAddress();
    }

    // Update TLAS build description with GPU-allocated resources
    tlasBuildDesc.DestAccelerationStructureData     = m_topLevelAccelerationStructure.resource->GetGPUVirtualAddress();
    tlasBuildDesc.ScratchAccelerationStructureData  = scratchResourceTlas.resource->GetGPUVirtualAddress();
    tlasBuildDesc.Inputs.InstanceDescs              = blasInstanceDescsBuffer.resource->GetGPUVirtualAddress();

    // Build acceleration structures.
    // BLASes
    for (size_t i = 0ULL; i < num_objects; i++) {
        m_dxrCommandList->BuildRaytracingAccelerationStructure(&blasBuildDescs[i], 0, nullptr);
        CD3DX12_RESOURCE_BARRIER bvh_uav = CD3DX12_RESOURCE_BARRIER::UAV(m_bottomLevelAccelerationStructures[i].resource.Get());
        m_dxrCommandList->ResourceBarrier(1, &bvh_uav);
    }
    // TLAS
    m_dxrCommandList->BuildRaytracingAccelerationStructure(&tlasBuildDesc, 0, nullptr);
    
    // Kick off acceleration structure construction.
    m_deviceResources->ExecuteCommandList();

    // Wait for GPU to finish as the locally created temporary GPU resources will get released once we go out of scope.
    m_deviceResources->WaitForGpu();
}

void D3D12RaytracingSimpleLighting::BuildLightBuffers()
{
    D3D12MA::Allocator* allocator               = m_deviceResources->GetD3DMAllocator();
    ID3D12GraphicsCommandList* commandList      = m_deviceResources->GetCommandList();
    ID3D12CommandAllocator* commandAllocator    = m_deviceResources->GetCommandAllocator();

    // Reset the command list so we can issue copy command and resource transitions for staging buffer copies
    commandList->Reset(commandAllocator, nullptr);

    // TODO: Acquire these in a programmatic manner instead of just creating dummies
    std::vector<PointLight> pointLights;
    PointLight p0 = {
        .position = { 0.5f, 1.0f, -0.3f },
        .color = { 0.35f, 0.35f, 0.35f }
    };
    PointLight p1 = {
        .position = { -0.5f, 1.0f, 0.2f },
        .color = { 0.65f, 0.65f, 0.65f }
    };
    pointLights.push_back(p0);
    pointLights.push_back(p1);

    // Create device buffer, staging buffer, and an SRV for the device buffer
    D3DBuffer pointLightsStaging;
    size_t pointLightsSize = pointLights.size() * sizeof(PointLight);
    AllocateUploadBuffer(allocator, pointLights.data(), pointLightsSize, &pointLightsStaging.resource.resource, &pointLightsStaging.resource.allocation, L"PointLightsStaging");
    AllocateDeviceBuffer(allocator, pointLightsSize, &m_pointLightsBuffer.resource.resource, &m_pointLightsBuffer.resource.allocation, false, D3D12_RESOURCE_STATE_COPY_DEST, L"PointLights");
    CreateBufferSRV(&m_pointLightsBuffer, static_cast<UINT>(pointLights.size()), sizeof(PointLight), DescriptorHeapSlots::PointLightsBuffer);

    // Queue copies from staging buffer copies and transitions to SRV state
    commandList->CopyResource(m_pointLightsBuffer.resource.resource.Get(), pointLightsStaging.resource.resource.Get());
    CD3DX12_RESOURCE_BARRIER srvTransition = CD3DX12_RESOURCE_BARRIER::Transition(m_pointLightsBuffer.resource.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &srvTransition);

    // Kick off staging buffer copy and wait for GPU to finish as the locally created temporary GPU resources will get released once we go out of scope
    m_deviceResources->ExecuteCommandList();
    m_deviceResources->WaitForGpu();
}

// Build shader tables.
// This encapsulates all shader records - shaders and the arguments for their local root signatures.
void D3D12RaytracingSimpleLighting::BuildShaderTables()
{
    ID3D12Device* device            = m_deviceResources->GetD3DDevice();
    D3D12MA::Allocator* allocator   = m_deviceResources->GetD3DMAllocator();

    void* rayGenShaderIdentifier;
    void* missShaderIdentifier;
    void* hitGroupShaderIdentifier;

    auto GetShaderIdentifiers = [&](auto* stateObjectProperties)
    {
        rayGenShaderIdentifier = stateObjectProperties->GetShaderIdentifier(c_raygenShaderName);
        missShaderIdentifier = stateObjectProperties->GetShaderIdentifier(c_missShaderName);
        hitGroupShaderIdentifier = stateObjectProperties->GetShaderIdentifier(c_hitGroupName);
    };

    // Get shader identifiers.
    UINT shaderIdentifierSize;
    {
        ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
        ThrowIfFailed(m_dxrStateObject.As(&stateObjectProperties));
        GetShaderIdentifiers(stateObjectProperties.Get());
        shaderIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    }

    // Ray gen shader table
    {
        UINT numShaderRecords = 1;
        UINT shaderRecordSize = shaderIdentifierSize;
        ShaderTable rayGenShaderTable(device, allocator, numShaderRecords, shaderRecordSize, L"RayGenShaderTable");
        rayGenShaderTable.push_back(ShaderRecord(rayGenShaderIdentifier, shaderIdentifierSize));
        m_rayGenShaderTable = { rayGenShaderTable.GetAllocation(), rayGenShaderTable.GetResource() };
    }

    // Miss shader table
    {
        UINT numShaderRecords = 1;
        UINT shaderRecordSize = shaderIdentifierSize;
        ShaderTable missShaderTable(device, allocator, numShaderRecords, shaderRecordSize, L"MissShaderTable");
        missShaderTable.push_back(ShaderRecord(missShaderIdentifier, shaderIdentifierSize));
        m_missShaderTable = { missShaderTable.GetAllocation(), missShaderTable.GetResource() };
    }

    // Hit group shader table
    {
        UINT numShaderRecords = 1;
        UINT shaderRecordSize = shaderIdentifierSize;
        ShaderTable hitGroupShaderTable(device, allocator, numShaderRecords, shaderRecordSize, L"HitGroupShaderTable");
        hitGroupShaderTable.push_back(ShaderRecord(hitGroupShaderIdentifier, shaderIdentifierSize));
        m_hitGroupShaderTable = { hitGroupShaderTable.GetAllocation(), hitGroupShaderTable.GetResource() };
    }
}

// Update frame-based values.
void D3D12RaytracingSimpleLighting::OnUpdate()
{
    m_timer.Tick();
    CalculateFrameStats();
    float elapsedTime = static_cast<float>(m_timer.GetElapsedSeconds());
    auto frameIndex = m_deviceResources->GetCurrentFrameIndex();
    auto prevFrameIndex = m_deviceResources->GetPreviousFrameIndex();

    // Rotate the camera around Y axis.
    {
        float secondsToRotateAround = 24.0f;
        float angleToRotateBy = 360.0f * (elapsedTime / secondsToRotateAround);
        XMMATRIX rotate = XMMatrixRotationY(XMConvertToRadians(angleToRotateBy));
        m_eye = XMVector3Transform(m_eye, rotate);
        m_up = XMVector3Transform(m_up, rotate);
        m_at = XMVector3Transform(m_at, rotate);
        UpdateCameraMatrices();
    }
}

void D3D12RaytracingSimpleLighting::DoRaytracing()
{
    auto commandList    = m_deviceResources->GetCommandList();
    auto frameIndex     = m_deviceResources->GetCurrentFrameIndex();
    
    auto DispatchRays = [&](ID3D12GraphicsCommandList5* commandList, ID3D12StateObject* stateObject, D3D12_DISPATCH_RAYS_DESC* dispatchDesc)
    {
        // Since each shader table has only one shader record, the stride is same as the size.
        dispatchDesc->HitGroupTable.StartAddress = m_hitGroupShaderTable.resource->GetGPUVirtualAddress();
        dispatchDesc->HitGroupTable.SizeInBytes = m_hitGroupShaderTable.resource->GetDesc().Width;
        dispatchDesc->HitGroupTable.StrideInBytes = dispatchDesc->HitGroupTable.SizeInBytes;
        dispatchDesc->MissShaderTable.StartAddress = m_missShaderTable.resource->GetGPUVirtualAddress();
        dispatchDesc->MissShaderTable.SizeInBytes = m_missShaderTable.resource->GetDesc().Width;
        dispatchDesc->MissShaderTable.StrideInBytes = dispatchDesc->MissShaderTable.SizeInBytes;
        dispatchDesc->RayGenerationShaderRecord.StartAddress = m_rayGenShaderTable.resource->GetGPUVirtualAddress();
        dispatchDesc->RayGenerationShaderRecord.SizeInBytes = m_rayGenShaderTable.resource->GetDesc().Width;
        dispatchDesc->Width = m_width;
        dispatchDesc->Height = m_height;
        dispatchDesc->Depth = 1;
        commandList->SetPipelineState1(stateObject);
        commandList->DispatchRays(dispatchDesc);
    };

    // Bind the descriptor heap and root signature
    commandList->SetDescriptorHeaps(1, m_descriptorHeap.GetAddressOf());
    commandList->SetComputeRootSignature(m_raytracingGlobalRootSignature.Get());

    // Copy the updated scene constant buffer to GPU and bind it
    memcpy(&m_mappedConstantData[frameIndex].constants, &m_sceneCB[frameIndex], sizeof(m_sceneCB[frameIndex]));
    auto cbGpuAddress = m_perFrameConstants.resource->GetGPUVirtualAddress() + frameIndex * sizeof(m_mappedConstantData[0]);
    commandList->SetComputeRootConstantBufferView(BoundResourceSlots::SceneCB, cbGpuAddress);
   
    // Bind the acceleration structure and dispatch rays.
    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    commandList->SetComputeRootShaderResourceView(BoundResourceSlots::TLAS, m_topLevelAccelerationStructure.resource->GetGPUVirtualAddress());
    DispatchRays(m_dxrCommandList.Get(), m_dxrStateObject.Get(), &dispatchDesc);
}

// Update the application state with the new resolution.
void D3D12RaytracingSimpleLighting::UpdateForSizeChange(UINT width, UINT height)
{
    DXSample::UpdateForSizeChange(width, height);
}

// Copy the raytracing output to the backbuffer.
void D3D12RaytracingSimpleLighting::CopyRaytracingOutputToBackbuffer()
{
    auto commandList= m_deviceResources->GetCommandList();
    auto renderTarget = m_deviceResources->GetRenderTarget();

    D3D12_RESOURCE_BARRIER preCopyBarriers[2];
    preCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(renderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    preCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_raytracingOutput.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->ResourceBarrier(ARRAYSIZE(preCopyBarriers), preCopyBarriers);

    commandList->CopyResource(renderTarget, m_raytracingOutput.resource.Get());

    D3D12_RESOURCE_BARRIER postCopyBarriers[2];
    postCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(renderTarget, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    postCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_raytracingOutput.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->ResourceBarrier(ARRAYSIZE(postCopyBarriers), postCopyBarriers);
}

// Create resources that are dependent on the size of the main window.
void D3D12RaytracingSimpleLighting::CreateWindowSizeDependentResources()
{
    CreateRaytracingOutputResource(); 
    UpdateCameraMatrices();
}

// Release resources that are dependent on the size of the main window.
void D3D12RaytracingSimpleLighting::ReleaseWindowSizeDependentResources()
{
    m_raytracingOutput.resource.Reset();
    m_raytracingOutput.allocation.Reset();
}

// Release all resources that depend on the device.
void D3D12RaytracingSimpleLighting::ReleaseDeviceDependentResources()
{
    m_raytracingGlobalRootSignature.Reset();

    m_dxrDevice.Reset();
    m_dxrCommandList.Reset();
    m_dxrStateObject.Reset();
    m_descriptorHeap.Reset();
    m_descriptorsAllocated = 0;

    m_pointLightsBuffer.resource.resource.Reset();
    m_pointLightsBuffer.resource.allocation.Reset();
    m_perFrameConstants.resource.Reset();
    m_perFrameConstants.allocation.Reset();
    m_rayGenShaderTable.resource.Reset();
    m_rayGenShaderTable.allocation.Reset();
    m_missShaderTable.resource.Reset();
    m_missShaderTable.allocation.Reset();
    m_hitGroupShaderTable.resource.Reset();
    m_hitGroupShaderTable.allocation.Reset();
    for (size_t i = 0ULL; i < m_indexBuffers.size(); i++) {
        m_indexBuffers[i].resource.resource.Reset();
        m_vertexBuffers[i].resource.allocation.Reset();
        m_bottomLevelAccelerationStructures[i].resource.Reset();
        m_bottomLevelAccelerationStructures[i].allocation.Reset();
    }
    m_topLevelAccelerationStructure.resource.Reset();
    m_topLevelAccelerationStructure.allocation.Reset();
}

void D3D12RaytracingSimpleLighting::RecreateD3D()
{
    // Give GPU a chance to finish its execution in progress.
    try
    {
        m_deviceResources->WaitForGpu();
    }
    catch (HrException&)
    {
        // Do nothing, currently attached adapter is unresponsive.
    }
    m_deviceResources->HandleDeviceLost();
}

// Render the scene.
void D3D12RaytracingSimpleLighting::OnRender()
{
    if (!m_deviceResources->IsWindowVisible())
    {
        return;
    }

    m_deviceResources->Prepare();
    DoRaytracing();
    CopyRaytracingOutputToBackbuffer();

    m_deviceResources->Present(D3D12_RESOURCE_STATE_PRESENT);
}

void D3D12RaytracingSimpleLighting::OnDestroy()
{
    // Let GPU finish before releasing D3D resources.
    m_deviceResources->WaitForGpu();
    OnDeviceLost();
}

// Release all device dependent resouces when a device is lost.
void D3D12RaytracingSimpleLighting::OnDeviceLost()
{
    ReleaseWindowSizeDependentResources();
    ReleaseDeviceDependentResources();
}

// Create all device dependent resources when a device is restored.
void D3D12RaytracingSimpleLighting::OnDeviceRestored()
{
    CreateDeviceDependentResources();
    CreateWindowSizeDependentResources();
}

// Compute the average frames per second and million rays per second.
void D3D12RaytracingSimpleLighting::CalculateFrameStats()
{
    static int frameCnt = 0;
    static double elapsedTime = 0.0f;
    double totalTime = m_timer.GetTotalSeconds();
    frameCnt++;

    // Compute averages over one second period.
    if ((totalTime - elapsedTime) >= 1.0f)
    {
        float diff = static_cast<float>(totalTime - elapsedTime);
        float fps = static_cast<float>(frameCnt) / diff; // Normalize to an exact second.

        frameCnt = 0;
        elapsedTime = totalTime;

        float MRaysPerSecond = (m_width * m_height * fps) / static_cast<float>(1e6);

        wstringstream windowText;
        windowText << setprecision(2) << fixed
            << L"    fps: " << fps << L"     ~Million Primary Rays/s: " << MRaysPerSecond
            << L"    GPU[" << m_deviceResources->GetAdapterID() << L"]: " << m_deviceResources->GetAdapterDescription();
        SetCustomWindowText(windowText.str().c_str());
    }
}

// Handle OnSizeChanged message event.
void D3D12RaytracingSimpleLighting::OnSizeChanged(UINT width, UINT height, bool minimized)
{
    if (!m_deviceResources->WindowSizeChanged(width, height, minimized))
    {
        return;
    }

    UpdateForSizeChange(width, height);

    ReleaseWindowSizeDependentResources();
    CreateWindowSizeDependentResources();
}

// Allocate a descriptor and return its index. 
// If the passed descriptorIndexToUse is valid, it will be used instead of allocating a new one.
UINT D3D12RaytracingSimpleLighting::AllocateDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpuDescriptor, UINT descriptorIndexToUse)
{
    auto descriptorHeapCpuBase = m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    if (descriptorIndexToUse >= m_descriptorHeap->GetDesc().NumDescriptors)
    {
        descriptorIndexToUse = m_descriptorsAllocated++;
    }
    *cpuDescriptor = CD3DX12_CPU_DESCRIPTOR_HANDLE(descriptorHeapCpuBase, descriptorIndexToUse, m_descriptorSize);
    return descriptorIndexToUse;
}

// Create SRV for a buffer.
UINT D3D12RaytracingSimpleLighting::CreateBufferSRV(D3DBuffer* buffer, UINT numElements, UINT elementSize, UINT descriptorIndexToUse)
{
    auto device = m_deviceResources->GetD3DDevice();

    // SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.NumElements = numElements;
    if (elementSize == 0)
    {
        srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        srvDesc.Buffer.StructureByteStride = 0;
    }
    else
    {
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        srvDesc.Buffer.StructureByteStride = elementSize;
    }
    UINT descriptorIndex = AllocateDescriptor(&buffer->cpuDescriptorHandle, descriptorIndexToUse);
    device->CreateShaderResourceView(buffer->resource.resource.Get(), &srvDesc, buffer->cpuDescriptorHandle);
    buffer->gpuDescriptorHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), descriptorIndex, m_descriptorSize);
    return descriptorIndex;
}