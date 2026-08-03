// stereo_config.h - [Stereo3D] config + live hotkeys for GBFR
#pragma once
#include <windows.h>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>

struct StereoCfg {
    int   enabled      = 1;      // 3D ON by default
    int   outputMode   = 1;      // DEFAULT SBS. 0 off/mono | 1 SBS | 2 TAB | 3 Line | 4 Column | 5 Checker | 6 VR full-res
    int   swapEyes     = 0;      // flip if depth looks inverted

    // ---- PRIMARY controls ----
    float separationMM       = 65.0f;   // 3D strength = IPD in mm (55-75)
    float convergenceDistance= 4.0f;    // distance (world units) where things sit ON screen
    float convergenceScale   = 0.50f;   // per-game calibration
    // advanced fallbacks (used only when primary above is 0)
    float separationX        = 0.0f;
    float convergenceManual  = 0.0f;

    // ---- derived (what the shader actually consumes) ----
    float worldUnitsPerMetre = 1.0f;    // GBFR: 1 world unit = 1 metre
    float sepUnits           = 0.0f;    // per-eye offset in world units
    float afrConv            = 0.0f;    // per-eye convergence HIT (uv)

    // ---- AFR stability ----
    float afrStaleMs   = 250.0f;
    float afrStaleFactor = 1.5f;
    int   afrReproject = 0;
    float afrReprojGain= 0.011f;
    float afrReprojClamp=0.06f;
    int   presentSyncEyes = 1;

    // ---- present-side ----
    int   uncap        = 1;
    int   maxFps       = 0;
    int   maxFrameLatency = 1;

    // ---- live-tune steps ----
    float sepStepMM    = 3.0f;
    float convStepDist = 3.0f;

    // ---- keys ----
    int   adjustMod    = 0x11;   // VK_CONTROL
    int   sepDownKey   = 0x72, sepUpKey  = 0x73; // F3 / F4
    int   convDownKey  = 0x74, convUpKey = 0x75; // F5 / F6
    int   modeCycleKey = 0x7A;                    // F11
    int   swapKey      = 0x7B;                    // F12
    int   reverseMod   = 0x10;   // VK_SHIFT
};

static void StereoRecompute(StereoCfg& c) {
    c.sepUnits = (c.separationMM > 0.f) ? (c.separationMM * 0.0005f * c.worldUnitsPerMetre)
                                        : c.separationX;
    c.afrConv  = (c.convergenceDistance > 0.f)
                 ? (c.convergenceScale * (c.sepUnits / c.convergenceDistance))
                 : c.convergenceManual;
}

static int StereoReadInt(const char* sec, const char* key, int def, const char* ini) {
    char buf[64] = {0};
    if (!GetPrivateProfileStringA(sec, key, "", buf, 64, ini) || !buf[0]) return def;
    char* p = buf;
    while (*p == ' ' || *p == '\t') ++p;
    int sign = 1;
    if (*p == '-') { sign = -1; ++p; } else if (*p == '+') { ++p; }
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    char* endp = nullptr;
    long v = strtol(p, &endp, base);
    if (endp == p) return def;
    return (int)(sign * v);
}

static void StereoLoad(StereoCfg& c, const char* ini) {
    auto gi = [&](const char* sec, const char* k, int d) { return StereoReadInt(sec, k, d, ini); };
    auto gf = [&](const char* sec, const char* k, float d) {
        char b[64], db[64];
        snprintf(db, 64, "%.5f", d);
        GetPrivateProfileStringA(sec, k, db, b, 64, ini);
        return (float)atof(b);
    };
    c.worldUnitsPerMetre = gf("Stereo3D", "WorldUnitsPerMetre", c.worldUnitsPerMetre);
    c.enabled            = gi("Stereo3D", "Enabled", c.enabled);
    c.outputMode         = gi("Stereo3D", "OutputMode", c.outputMode);
    c.separationMM       = gf("Stereo3D", "SeparationMM", c.separationMM);
    c.convergenceDistance = gf("Stereo3D", "ConvergenceDistance", c.convergenceDistance);
    c.convergenceScale   = gf("Stereo3D", "ConvergenceScale", c.convergenceScale);
    c.separationX        = gf("Stereo3D", "SeparationX", c.separationX);
    c.convergenceManual  = gf("Stereo3D", "Convergence", c.convergenceManual);
    c.swapEyes           = gi("Stereo3D", "SwapEyes", c.swapEyes);
    c.afrStaleMs         = gf("Stereo3D", "AfrStaleMs", c.afrStaleMs);
    c.afrStaleFactor     = gf("Stereo3D", "AfrStaleFactor", c.afrStaleFactor);
    c.afrReproject       = gi("Stereo3D", "AfrReproject", c.afrReproject);
    c.afrReprojGain      = gf("Stereo3D", "AfrReprojectGain", c.afrReprojGain);
    c.afrReprojClamp     = gf("Stereo3D", "AfrReprojectClamp", c.afrReprojClamp);
    c.presentSyncEyes    = gi("Stereo3D", "PresentSyncEyes", c.presentSyncEyes);
    c.uncap              = gi("Stereo3D", "PresentUncap", c.uncap);
    c.maxFps             = gi("Stereo3D", "MaxFPS", c.maxFps);
    c.maxFrameLatency    = gi("Stereo3D", "MaxFrameLatency", c.maxFrameLatency);
    c.sepStepMM          = gf("Stereo3D", "SeparationStep", c.sepStepMM);
    c.convStepDist       = gf("Stereo3D", "ConvergenceStep", c.convStepDist);
    c.adjustMod          = gi("Stereo3D", "AdjustModifier", c.adjustMod);
    c.sepDownKey         = gi("Stereo3D", "SeparationDownKey", c.sepDownKey);
    c.sepUpKey           = gi("Stereo3D", "SeparationUpKey", c.sepUpKey);
    c.convDownKey        = gi("Stereo3D", "ConvergenceDownKey", c.convDownKey);
    c.convUpKey          = gi("Stereo3D", "ConvergenceUpKey", c.convUpKey);
    c.modeCycleKey       = gi("Stereo3D", "ModeCycleKey", c.modeCycleKey);
    c.swapKey            = gi("Stereo3D", "SwapEyesKey", c.swapKey);
    c.reverseMod         = gi("Stereo3D", "ReverseModifier", c.reverseMod);
    StereoRecompute(c);
}

static void StereoSaveLive(const StereoCfg& c, const char* ini) {
    char b[64];
    snprintf(b, 64, "%.2f", c.separationMM);        WritePrivateProfileStringA("Stereo3D", "SeparationMM", b, ini);
    snprintf(b, 64, "%.2f", c.convergenceDistance); WritePrivateProfileStringA("Stereo3D", "ConvergenceDistance", b, ini);
    snprintf(b, 64, "%d", c.outputMode);           WritePrivateProfileStringA("Stereo3D", "OutputMode", b, ini);
    snprintf(b, 64, "%d", c.swapEyes);             WritePrivateProfileStringA("Stereo3D", "SwapEyes", b, ini);
}

struct KRep { bool prev = false; DWORD t0 = 0, last = 0; };

static bool RepFire(KRep& k, bool down) {
    const DWORD DELAY = 400, RATE = 90;
    DWORD now = GetTickCount();
    bool fire = false;
    if (down && !k.prev) { fire = true; k.t0 = k.last = now; }
    else if (down && now - k.t0 >= DELAY && now - k.last >= RATE) { fire = true; k.last = now; }
    k.prev = down;
    return fire;
}

static bool StereoPollHotkeys(StereoCfg& c, const char* ini) {
    static KRep sD, sU, cD, cU, mC, sw;
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool mod = (c.adjustMod == 0) || down(c.adjustMod);
    bool changed = false;
    const float dir = ((c.reverseMod != 0) && down(c.reverseMod)) ? -1.0f : 1.0f;
    static bool anyHeld = false;
    bool held = false;

    if (RepFire(sD, mod && down(c.sepDownKey))) { c.separationMM = fmaxf(0.f, c.separationMM - c.sepStepMM); changed = true; }
    if (RepFire(sU, mod && down(c.sepUpKey)))  { c.separationMM = fmaxf(0.f, c.separationMM + dir * c.sepStepMM); changed = true; }
    if (RepFire(cD, mod && down(c.convDownKey))) { c.convergenceDistance = fmaxf(0.5f, c.convergenceDistance / (1.0f + 0.03f * c.convStepDist)); changed = true; }
    if (RepFire(cU, mod && down(c.convUpKey)))  { c.convergenceDistance = fmaxf(0.5f, c.convergenceDistance * (1.0f + 0.03f * c.convStepDist * dir)); changed = true; }
    if (RepFire(mC, mod && down(c.modeCycleKey))) { c.outputMode = (c.outputMode + 1) % 7; changed = true; }
    if (RepFire(sw, mod && down(c.swapKey)))    { c.swapEyes = !c.swapEyes; changed = true; }

    if (changed) StereoRecompute(c);
    held = (mod && (down(c.sepDownKey) || down(c.sepUpKey) || down(c.convDownKey) || down(c.convUpKey)));
    if (anyHeld && !held) StereoSaveLive(c, ini);
    if (changed && (down(c.modeCycleKey) || down(c.swapKey))) StereoSaveLive(c, ini);
    anyHeld = held;
    return changed;
}
