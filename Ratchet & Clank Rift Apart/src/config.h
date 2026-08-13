#pragma once
// ============================================================================
//  Rift Apart 3D+6DOF - Configuration, INI load/save, hotkey helpers
//  Four independent layers: Tracking, Stereo3D, FOV, ZAxis
// ============================================================================
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <atomic>

// StereoMath helpers are in stereo_math.h (include separately to avoid redefinition)

// ---- edge-triggered key repeat (§23.3) ----
struct KRep {
    bool prev = false;
    DWORD t0 = 0, last = 0;
    static const DWORD DELAY = 400, RATE = 90; // ms
    bool Fire(bool down) {
        DWORD now = GetTickCount();
        bool fire = false;
        if (down && !prev) { fire = true; t0 = last = now; }
        else if (down && now - t0 >= DELAY && now - last >= RATE) { fire = true; last = now; }
        prev = down;
        return fire;
    }
};

// ---- full config struct ----
struct Config {
    // [Stereo3D] - LAYER 2
    int    stereoEnabled = 1;
    int    outputMode = 1;           // 0=off, 1=SBS, 2=TAB, 3=line, 4=col, 5=checker, 6=VR
    float  separationMM = 65.0f;
    float  convergenceDistance = 2.0f;
    float  convergenceScale = 0.5f;   // 0=pure shear, 1=pure HIT, 0.5=balanced (recommended)
    int    swapEyes = 0;
    // AFR stability
    float  afrStaleMs = 250.f;
    float  afrStaleFactor = 1.5f;
    int    afrReproject = 0;
    float  afrReprojectGain = 0.011f;
    float  afrReprojectClamp = 0.06f;
    int    presentSyncEyes = 1;
    int    maxFrameLatency = 1;
    int    presentUncap = 1;
    int    maxFPS = 0;
    // advanced fallbacks
    float  separationX = 0.f;       // raw units/eye, only if separationMM=0
    float  convergence = 0.05f;     // direct UV HIT (default 0.05 = strong convergence for half-res modes)
    // hotkeys
    int    adjustModifier = 0x11;    // VK_CONTROL
    int    reverseModifier = 0x10;   // VK_SHIFT
    int    separationDownKey = 0x72; // F3
    int    separationUpKey = 0x73;   // F4
    float  separationStep = 3.0f;
    int    convergenceDownKey = 0x74;// F5
    int    convergenceUpKey = 0x75;  // F6
    float  convergenceStep = 1.0f;  // 3% per tap (GBFR-style: 1.0 + 0.03*1.0 = 1.03)
    int    modeCycleKey = 0x7A;      // F11
    int    swapEyesKey = 0x7B;       // F12

    // [FOV] - LAYER 3 (extended from tracking-only)
    int    fovEnabled = 0;
    float  fovScale = 1.0f;
    float  fovOffsetDeg = 0.f;
    float  fovValue = 0.f;          // legacy fixed override
    int    fovOffset = 0x70;        // struct offset
    float  fovStep = 0.05f;
    int    fovUpKey = 0x77;         // F8
    int    fovDownKey = 0x76;       // F7

    // [ZAxis] - LAYER 4
    int    zEnabled = 1;
    float  zOffset = 0.f;           // metres
    float  zStep = 0.15f;
    int    zNearKey = 0x26;         // Up arrow
    int    zFarKey = 0x28;          // Down arrow

    // [Network]
    int    port = 4242;
    // [General]
    int    autoEnable = 1;
    int    diagnostics = 0;
    // [Hotkeys]
    int    hotkeysEnabled = 1;
    int    toggleKey = VK_END;
    int    recenterKey = VK_HOME;
    // [Test]
    int    wobbleTest = 0;
    int    wobbleMode = 1;
    float  wobbleDeg = 12.f;
    float  wobbleLean = 0.5f;
    float  wobbleHz = 0.4f;
    // [Rotation]
    float  yawSens = 1, pitchSens = 1, rollSens = 1, globalScale = 1, limitDeg = 80;
    int    invYaw = 0, invPitch = 0, invRoll = 1;
    // [Camera]
    int    camRotEnabled = 1, camPosEnabled = 1, matrixTranspose = 0;
    float  camRotScale = 1;
    int    matrixOffset = 0x0C;
    // [Position]
    float  leanScale = 2, leanScaleX = 2, leanScaleY = 2, leanScaleZ = 3, leanLimit = 6;
    int    invX = 1, invY = 1, invZ = 1, swapYZ = 0;
    float  worldUnitsPerMetre = 1.0f;  // TBD: calibrate for Rift Apart
    // [Filter]
    int    smoothMode = 1;
    float  smoothHz = 10, smoothEMA = 0.35f;
    // [Position] continued
    int    softLimit = 1;
};

extern Config g_cfg;
extern std::atomic<bool> g_tog, g_rec;
extern bool g_en;

// INI helpers
std::string ConfigIniPath();
int    GI(const char* s, const char* k, int d);
float  GF(const char* s, const char* k, float d);
int    GH(const char* s, const char* k, int d);
int    GK(const char* s, const char* k, int d);
void   ConfigLoad();
void   ConfigSaveValue(const char* section, const char* key, const char* value);
void   ConfigSaveFloat(const char* section, const char* key, float v);
void   ConfigSaveInt(const char* section, const char* key, int v);

// Self-module accessor (defined in config.cpp, set from DllMain)
void   SetSelfModuleCfg(HMODULE h);

// Live-tuning: process hotkeys, write back changed values
void ConfigProcessHotkeys();
