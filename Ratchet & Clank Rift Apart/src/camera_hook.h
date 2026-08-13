#pragma once
// ============================================================================
//  Rift Apart 3D+6DOF - Camera hook (AOB scan, cave, ApplyHeadRCRA)
//  Tier B: 4x4 world matrix @ struct+0x0C, FOV @ +0x70
//  Applies: head tracking rotation/lean, stereo eye offset, Z-dolly
// ============================================================================
#include <cstdint>
#include <atomic>

// Camera struct pointer (published by the cave assembler)
extern "C" volatile uint64_t g_cam;

// Install the camera AOB hook; returns true on success
bool CameraHook_Install(uint8_t* mod, size_t sz);

// Check if hook is still installed
bool CameraHook_IsInstalled();

// Reinstall hook if it was removed by the game
// force=true bypasses IsInstalled() check to re-scan for additional AOB locations
// (use when game switches to a different camera code path)
bool CameraHook_Reinstall(bool force = false);

// Install the FOV AOB hook
bool FovHook_Install(uint8_t* mod, size_t sz);

// Per-frame camera update (tracking smoothing, wobble)
void CameraUpdate();

// The function called from the code cave (C linkage)
extern "C" __attribute__((used,noinline)) void ApplyHeadRCRA(uint64_t cam);
