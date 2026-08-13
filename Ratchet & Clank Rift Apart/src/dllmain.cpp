// ============================================================================
//  Ratchet & Clank: Rift Apart - 3D+6DOF (DX12, 64-bit)
//  Four independent layers: Tracking (L1), Stereo3D (L2), FOV (L3), ZAxis (L4)
//  Camera: Tier B - 4x4 world matrix @ struct+0x0C, FOV @ +0x70
//  AFR stereo: alternate-frame capture + always-composited output
// ============================================================================
#include <winsock2.h>
#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include "config.h"
#include "logger.h"
#include "opentrack_receiver.h"
#include "camera_hook.h"
#include "afr_engine.h"
#include "dx12_hooks.h"
#include "compositor.h"

using namespace P5HT;

// ---- Global state ----
OpenTrackReceiver g_recv;

// FOV hook state (shared with camera_hook.cpp)
extern "C" { volatile uint64_t g_fovPtr=0; volatile int g_fovOn=0; volatile float g_fovVal=0.f; }

// ---- Main thread ----
static DWORD WINAPI Main(LPVOID) {
    // ---- single-instance guard ----
    HANDLE mtx = CreateMutexA(NULL, TRUE, "Global\\RiftApartHeadTracking_SingleInstance");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) { return 0; }

    Log("=== RiftApart-3D-6DOF loading (build %s %s) ===", __DATE__, __TIME__);

    // ---- Load config ----
    ConfigLoad();

    // ---- Install DX12 hooks FIRST (BEFORE game creates swapchain) ----
    // The game calls CreateDXGIFactory -> CreateSwapChain during the first 6 seconds.
    // We must patch dxgi.dll exports BEFORE that happens.
    if (!Dx12Hooks_Install()) {
        Log("[3D] WARNING: DX12 hook installation failed");
    }

    // ---- Start OpenTrack receiver (L1: tracking) ----
    if (!g_recv.Start((uint16_t)g_cfg.port)) Log("WARN receiver failed");

    // Now wait for the game to initialize and create its swapchain
    Sleep(6000);

    // ---- Install camera AOB hook (L1+L2+L3+L4) ----
    MODULEINFO mi;
    GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(0), &mi, sizeof(mi));
    uint8_t* mod = (uint8_t*)mi.lpBaseOfDll;
    size_t msz = mi.SizeOfImage;

    int tries = 0;
    while (!CameraHook_Install(mod, msz) && tries < 120) { Sleep(500); tries++; }
    if (tries >= 120) Log("hook: GAVE UP - camera AOB never matched");

    // ---- Install FOV hook (L3) ----
    { int ft = 0; while (!FovHook_Install(mod, msz) && ft < 120) { Sleep(500); ft++; } }

    // ---- Wait for DX12 objects (swapchain, device, queue) ----
    // The swapchain should have been captured by now (via our dxgi export hook).
    // The command queue may arrive later.
    int dx12Tries = 0;
    while (!Dx12_IsReady() && dx12Tries < 120) { Sleep(500); dx12Tries++; }
    if (Dx12_IsReady()) {
        Compositor_Init();
        Log("[3D] Compositor ready");
    } else {
        Log("[3D] DX12 objects not captured - stereo disabled (tracking still works)");
    }

    // ---- Enable tracking if configured ----
    g_en = g_cfg.autoEnable ? true : false;

    // ---- Initialize AFR ----
    if (g_cfg.stereoEnabled) {
        Afr_Reset();
        Log("[3D] AFR engine initialized (mode=%d, sep=%.1fmm, conv=%.2f, WUPM=%.1f)",
            g_cfg.outputMode, g_cfg.separationMM, g_cfg.convergenceDistance, g_cfg.worldUnitsPerMetre);
    }

    // ---- Startup banner with build timestamp + wall clock ----
    // __DATE__ __TIME__ = compile time (identifies the build version)
    // Wall clock = when the user actually launched the game this session
    {
        time_t now = time(nullptr);
        struct tm tmNow;
        localtime_s(&tmNow, &now);
        char timeBuf[64];
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tmNow);
        Log("=============== RIFTAPART STEREO BRING-UP (DX12) ===============");
        Log("[3D] build %s %s  session-start %s", __DATE__, __TIME__, timeBuf);
        Log("[3D] outputMode=%d  sep=%.1fmm  conv=%.2f  scale=%.2f  WUPM=%.1f",
            g_cfg.outputMode,
            g_cfg.separationMM, g_cfg.convergenceDistance, g_cfg.convergenceScale, g_cfg.worldUnitsPerMetre);
        Log("[3D] 3D=%s  Wobble=%s  DLSS note: disable for menu text clarity",
            g_cfg.stereoEnabled ? "ON" : "off", g_cfg.wobbleTest ? "ON" : "off");
        Log("=================================================================");
    }

    Log("RiftApart-3D-6DOF active. 3D=%s Wobble %s.",
        g_cfg.stereoEnabled ? "ON" : "off", g_cfg.wobbleTest ? "ON" : "off");

    // ---- Main loop: hotkeys + camera update + FOV + pacing ----
    while (true) {
        ConfigProcessHotkeys();

        // Camera update (tracking smoothing, wobble)
        CameraUpdate();

        // FOV layer (L3)
        g_fovOn = (g_cfg.fovEnabled && g_cfg.fovValue > 0.f) ? 1 : 0;
        g_fovVal = g_cfg.fovValue;

        // Show current game FOV once
        { static int shown = 0;
          if (!shown && g_fovPtr) {
              shown = 1;
              float* f = (float*)(g_fovPtr + g_cfg.fovOffset);
              if (!IsBadReadPtr(f, 4))
                  Log("fov: game FOV is currently %.4f  (set [FOV] Value near this)", *f);
          }
        }

        Sleep(4);
    }
}

// ---- DLL entry point ----
BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        SetSelfModule(h);
        SetSelfModuleCfg(h);
        CreateThread(0, 0, Main, 0, 0, 0);
    }
    return TRUE;
}
