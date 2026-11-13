// Compiled as C on purpose: in C mode d3d12.h / dxgi*.h expose the plain
// vtable structs (ID3D12DeviceVtbl etc.), so slot index = offsetof / sizeof(void*).
#include <stddef.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include "vtbl_index.h"

#define IDX(vt, m) ((int)(offsetof(vt, m) / sizeof(void*)))

const VtblIndex g_vtbl = {
    IDX(ID3D12DeviceVtbl, CreateCommandQueue),
    IDX(ID3D12DeviceVtbl, CreateCommandAllocator),
    IDX(ID3D12DeviceVtbl, CreateCommandList),
    IDX(ID3D12DeviceVtbl, CreateConstantBufferView),
    IDX(ID3D12DeviceVtbl, CreateShaderResourceView),
    IDX(ID3D12DeviceVtbl, CreateCommittedResource),
    IDX(ID3D12DeviceVtbl, CreateHeap),
    IDX(ID3D12DeviceVtbl, CreatePlacedResource),
    IDX(ID3D12Device4Vtbl, CreateCommittedResource1),
    IDX(ID3D12Device4Vtbl, CreateHeap1),
    IDX(ID3D12Device8Vtbl, CreateCommittedResource2),
    IDX(ID3D12Device8Vtbl, CreatePlacedResource1),
    IDX(ID3D12Device10Vtbl, CreateCommittedResource3),
    IDX(ID3D12Device10Vtbl, CreatePlacedResource2),

    IDX(ID3D12CommandQueueVtbl, ExecuteCommandLists),

    IDX(ID3D12GraphicsCommandListVtbl, Close),
    IDX(ID3D12GraphicsCommandListVtbl, Reset),
    IDX(ID3D12GraphicsCommandListVtbl, DrawInstanced),
    IDX(ID3D12GraphicsCommandListVtbl, DrawIndexedInstanced),
    IDX(ID3D12GraphicsCommandListVtbl, Dispatch),
    IDX(ID3D12GraphicsCommandListVtbl, CopyBufferRegion),
    IDX(ID3D12GraphicsCommandListVtbl, CopyResource),
    IDX(ID3D12GraphicsCommandListVtbl, IASetIndexBuffer),
    IDX(ID3D12GraphicsCommandListVtbl, IASetVertexBuffers),
    IDX(ID3D12GraphicsCommandListVtbl, SetGraphicsRootConstantBufferView),
    IDX(ID3D12GraphicsCommandListVtbl, SetComputeRootConstantBufferView),
    IDX(ID3D12GraphicsCommandListVtbl, SetGraphicsRootShaderResourceView),
    IDX(ID3D12GraphicsCommandListVtbl, SetComputeRootShaderResourceView),
    IDX(ID3D12GraphicsCommandListVtbl, ExecuteIndirect),

    IDX(ID3D12ResourceVtbl, Map),
    IDX(ID3D12ResourceVtbl, Unmap),
    IDX(ID3D12ResourceVtbl, GetHeapProperties),

    IDX(ID3D12HeapVtbl, GetDesc),

    IDX(IDXGIFactoryVtbl, CreateSwapChain),
    IDX(IDXGIFactory2Vtbl, CreateSwapChainForHwnd),
    IDX(IDXGISwapChainVtbl, Present),
    IDX(IDXGISwapChain1Vtbl, Present1),
};
