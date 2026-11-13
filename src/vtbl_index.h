// COM vtable slot indices, computed in vtbl_index.c from the C interface
// definitions of d3d12.h / dxgi*.h (offsetof on the *Vtbl structs), so we
// never hardcode slot numbers by hand.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VtblIndex {
    // ID3D12Device / Device4 / Device8 / Device10
    int Device_CreateCommandQueue;
    int Device_CreateCommandAllocator;
    int Device_CreateCommandList;
    int Device_CreateConstantBufferView;
    int Device_CreateShaderResourceView;
    int Device_CreateCommittedResource;
    int Device_CreateHeap;
    int Device_CreatePlacedResource;
    int Device4_CreateCommittedResource1;
    int Device4_CreateHeap1;
    int Device8_CreateCommittedResource2;
    int Device8_CreatePlacedResource1;
    int Device10_CreateCommittedResource3;
    int Device10_CreatePlacedResource2;

    // ID3D12CommandQueue
    int Queue_ExecuteCommandLists;

    // ID3D12GraphicsCommandList
    int CL_Close;
    int CL_Reset;
    int CL_DrawInstanced;
    int CL_DrawIndexedInstanced;
    int CL_Dispatch;
    int CL_CopyBufferRegion;
    int CL_CopyResource;
    int CL_IASetIndexBuffer;
    int CL_IASetVertexBuffers;
    int CL_SetGraphicsRootConstantBufferView;
    int CL_SetComputeRootConstantBufferView;
    int CL_SetGraphicsRootShaderResourceView;
    int CL_SetComputeRootShaderResourceView;
    int CL_ExecuteIndirect;

    // ID3D12Resource
    int Resource_Map;
    int Resource_Unmap;
    int Resource_GetHeapProperties;

    // ID3D12Heap
    int Heap_GetDesc;

    // DXGI
    int Factory_CreateSwapChain;
    int Factory2_CreateSwapChainForHwnd;
    int SwapChain_Present;
    int SwapChain1_Present1;
} VtblIndex;

extern const VtblIndex g_vtbl;

#ifdef __cplusplus
}
#endif
