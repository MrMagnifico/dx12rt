#pragma once

#include "d3d12ma/D3D12MemAlloc.h"
#include <wrl.h>

using Microsoft::WRL::ComPtr;

struct D3DResource {
    ComPtr<D3D12MA::Allocation> allocation;
    ComPtr<ID3D12Resource> resource;
};

struct D3DBuffer {
    D3DResource d3d_resource;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptorHandle;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptorHandle;
};

struct D3DRenderTarget {
    D3DResource d3d_resource;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptorHandle;
};
