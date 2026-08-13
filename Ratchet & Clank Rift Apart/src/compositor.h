#pragma once
// ============================================================================
//  Rift Apart 3D+6DOF - Compositor (DX12 pipeline, eye slots, composite draw)
// ============================================================================
struct IDXGISwapChain3;

// ---- Initialization (called once device+swapchain+queue are captured) ----
bool Compositor_Init();
bool Compositor_IsReady();
void Compositor_Cleanup();

// ---- Execute the composite pass (called from Present hook) ----
void Compositor_Execute(IDXGISwapChain3* sw, int eye);
