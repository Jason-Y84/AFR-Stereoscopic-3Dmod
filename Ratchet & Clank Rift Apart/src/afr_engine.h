#pragma once
// ============================================================================
//  Rift Apart 3D+6DOF - AFR Engine (eye clock, eye-queue, stability stack)
//  API-agnostic state: Present/ECL hooks call into this, compositor reads it
// ============================================================================
#include <atomic>
#include <cstdint>

// ---- Eye clock: PresentCount-edge flip (§13c) ----
// Returns the eye the camera should render THIS frame
int  Afr_FrameEye();
// Camera cave reports which eye it just rendered (pushes to FIFO)
void Afr_ReportCameraEye(int eye);

// ---- Eye-queue FIFO (§13) ----
// Pop the eye belonging to the frame now on screen
int  Afr_PopFrameEye();

// ---- Warmup gate (§15.2) ----
// Returns true if still warming up; caller should forceMono
bool Afr_IsWarmup();
void Afr_ResetWarmup();

// ---- Fresh/stale detection (§15.5) ----
int  Afr_FreshSlot();   // returns 0 or 1
int  Afr_StaleSlot();   // returns 0 or 1

// ---- Idle guard (§15.10) ----
// Call from Present: pass whether camera wrote this frame
bool Afr_IdleCheck(int wrote);
// Returns true if we are in idle passthrough mode
bool Afr_IsIdle();

// ---- Reprojection (§15.6) ----
// Update viewpoint yaw per slot (called when capturing an eye)
void Afr_SetSlotYaw(int eye, float yaw);
// Get the reprojection shift for a slot
float Afr_GetReprojShift(int slot);

// ---- Slot timestamps ----
void Afr_SetSlotTime(int eye, double ms);
double Afr_SlotTimeMs(int slot);

// ---- Cadence log (§16) ----
void Afr_TickCadence();
// Note that a camera write happened this frame (for cadence stats)
void Afr_NoteCamWrite();

// ---- QPC timing helper (§15.3) ----
double QpcNowMs();

// ---- Pipeline depth pin (§13b) ----
// Called from compositor after signaling the fence
void Afr_SetPipelineDepth(int depth);

// ---- Per-frame state: must be called once per displayed frame ----
void Afr_BeginFrame();  // resets g_camWrote latch, etc.

// ---- Full reset (on enable/disable) ----
void Afr_Reset();
