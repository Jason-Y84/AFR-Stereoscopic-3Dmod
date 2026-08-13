#pragma once
// ============================================================================
//  Rift Apart 3D+6DOF - DX12 Hooks header
//  Forward-declares D3D12 types to avoid pulling in d3d12.h here
//  (source files that need IID definitions must define INITGUID before
//   including d3d12.h themselves, or include iids.cpp)
// ============================================================================
#include <windows.h>

// Forward declarations of D3D12/DXGI interface types
struct IDXGISwapChain3;
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12CommandList;

// ---- Hook installation ----
// Creates dummy D3D12 objects, patches their vtable (shared across all instances),
// then releases them. Call BEFORE or AFTER game init - works either way.
bool Dx12Hooks_Install();

// ---- Accessors for captured objects (valid after first hook call) ----
IDXGISwapChain3*    Dx12_GetSwapChain();
ID3D12Device*       Dx12_GetDevice();
ID3D12CommandQueue* Dx12_GetDirectQueue();

// ---- Check if real swapchain+queue have been captured ----
bool Dx12_IsReady();

// ---- Accessors for compositor ----
// Execute command lists via the REAL ECL (bypasses our hook so our own
// composite draw is never mistaken for a game frame submission)
void Dx12_RealExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists);
int  Dx12_GetFrameEye();       // 0=LEFT, 1=RIGHT (present parity)

// ---- Cleanup ----
void Dx12Hooks_Cleanup();

// ---- AFR eye state (called by camera cave) ----
void Stereo_NoteCameraWrite();   // camera cave calls this every write
