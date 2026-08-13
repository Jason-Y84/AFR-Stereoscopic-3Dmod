// ============================================================================
//  Rift Apart 3D+6DOF - Configuration implementation
// ============================================================================
#include "config.h"
#include "logger.h"

using namespace P5HT;

Config g_cfg;
std::atomic<bool> g_tog{false}, g_rec{false};
bool g_en = false;

// ---- INI path ----
// Reuses P5HT::SelfModule() from logger.h (set via SetSelfModule in DllMain)
void SetSelfModuleCfg(HMODULE h) { P5HT::SetSelfModule(h); }

std::string ConfigIniPath() {
    char p[MAX_PATH];
    GetModuleFileNameA(P5HT::SelfModule(), p, MAX_PATH);
    char* s = strrchr(p, '\\');
    if (s) strcpy(s + 1, "3D-6DOF config.ini");
    return p;
}

int GI(const char* s, const char* k, int d) {
    return GetPrivateProfileIntA(s, k, d, ConfigIniPath().c_str());
}

float GF(const char* s, const char* k, float d) {
    char b[64], e[64];
    snprintf(e, 64, "%.6f", d);
    GetPrivateProfileStringA(s, k, e, b, 64, ConfigIniPath().c_str());
    return (float)atof(b);
}

int GH(const char* s, const char* k, int d) {
    char b[64];
    GetPrivateProfileStringA(s, k, "", b, 64, ConfigIniPath().c_str());
    std::string v(b);
    if (v.empty()) return d;
    if (v.rfind("0x", 0) == 0 || v.rfind("0X", 0) == 0) return (int)strtol(v.c_str(), 0, 16);
    return (int)strtol(v.c_str(), 0, 10);
}

int GK(const char* s, const char* k, int d) {
    char b[64];
    GetPrivateProfileStringA(s, k, "", b, 64, ConfigIniPath().c_str());
    std::string v(b);
    if (v.empty()) return d;
    struct { const char* n; int vk; } M[] = {
        {"END",VK_END},{"HOME",VK_HOME},{"INSERT",VK_INSERT},{"DELETE",VK_DELETE},
        {"PGUP",VK_PRIOR},{"PGDN",VK_NEXT},{"F1",VK_F1},{"F2",VK_F2},{"F3",VK_F3},
        {"F4",VK_F4},{"F5",VK_F5},{"F6",VK_F6},{"F7",VK_F7},{"F8",VK_F8},
        {"F9",VK_F9},{"F10",VK_F10},{"F11",VK_F11},{"F12",VK_F12},
        {"UP",VK_UP},{"DOWN",VK_DOWN},{"LEFT",VK_LEFT},{"RIGHT",VK_RIGHT}
    };
    for (auto& m : M) if (v == m.n) return m.vk;
    if (v.size() == 1) return v[0];
    if (v.rfind("0x", 0) == 0 || v.rfind("0X", 0) == 0) return (int)strtol(v.c_str(), 0, 16);
    return d;
}

void ConfigLoad() {
    Config& c = g_cfg;
    // [Stereo3D]
    c.stereoEnabled     = GI("Stereo3D", "Enabled", c.stereoEnabled);
    c.outputMode        = GI("Stereo3D", "OutputMode", c.outputMode);
    c.separationMM      = GF("Stereo3D", "SeparationMM", c.separationMM);
    c.convergenceDistance= GF("Stereo3D", "ConvergenceDistance", c.convergenceDistance);
    c.convergenceScale  = GF("Stereo3D", "ConvergenceScale", c.convergenceScale);
    c.swapEyes          = GI("Stereo3D", "SwapEyes", c.swapEyes);
    c.afrStaleMs        = GF("Stereo3D", "AfrStaleMs", c.afrStaleMs);
    c.afrStaleFactor    = GF("Stereo3D", "AfrStaleFactor", c.afrStaleFactor);
    c.afrReproject      = GI("Stereo3D", "AfrReproject", c.afrReproject);
    c.afrReprojectGain  = GF("Stereo3D", "AfrReprojectGain", c.afrReprojectGain);
    c.afrReprojectClamp = GF("Stereo3D", "AfrReprojectClamp", c.afrReprojectClamp);
    c.presentSyncEyes   = GI("Stereo3D", "PresentSyncEyes", c.presentSyncEyes);
    c.maxFrameLatency   = GI("Stereo3D", "MaxFrameLatency", c.maxFrameLatency);
    c.presentUncap      = GI("Stereo3D", "PresentUncap", c.presentUncap);
    c.maxFPS            = GI("Stereo3D", "MaxFPS", c.maxFPS);
    c.separationX       = GF("Stereo3D", "SeparationX", c.separationX);
    c.convergence       = GF("Stereo3D", "Convergence", c.convergence);
    c.adjustModifier    = GH("Stereo3D", "AdjustModifier", c.adjustModifier);
    c.reverseModifier   = GH("Stereo3D", "ReverseModifier", c.reverseModifier);
    c.separationDownKey = GH("Stereo3D", "SeparationDownKey", c.separationDownKey);
    c.separationUpKey   = GH("Stereo3D", "SeparationUpKey", c.separationUpKey);
    c.separationStep    = GF("Stereo3D", "SeparationStep", c.separationStep);
    c.convergenceDownKey= GH("Stereo3D", "ConvergenceDownKey", c.convergenceDownKey);
    c.convergenceUpKey  = GH("Stereo3D", "ConvergenceUpKey", c.convergenceUpKey);
    c.convergenceStep   = GF("Stereo3D", "ConvergenceStep", c.convergenceStep);
    c.modeCycleKey      = GH("Stereo3D", "ModeCycleKey", c.modeCycleKey);
    c.swapEyesKey       = GH("Stereo3D", "SwapEyesKey", c.swapEyesKey);

    // [FOV] - extended
    c.fovEnabled    = GI("FOV", "Enabled", c.fovEnabled);
    c.fovScale      = GF("FOV", "Scale", c.fovScale);
    c.fovOffsetDeg  = GF("FOV", "OffsetDegrees", c.fovOffsetDeg);
    c.fovValue      = GF("FOV", "Value", c.fovValue);
    c.fovOffset     = GH("FOV", "Offset", c.fovOffset);
    c.fovStep       = GF("FOV", "FovStep", c.fovStep);
    c.fovUpKey      = GH("FOV", "FovUpKey", c.fovUpKey);
    c.fovDownKey    = GH("FOV", "FovDownKey", c.fovDownKey);

    // [ZAxis]
    c.zEnabled  = GI("ZAxis", "Enabled", c.zEnabled);
    c.zOffset   = GF("ZAxis", "Offset", c.zOffset);
    c.zStep     = GF("ZAxis", "Step", c.zStep);
    c.zNearKey  = GH("ZAxis", "NearKey", c.zNearKey);
    c.zFarKey   = GH("ZAxis", "FarKey", c.zFarKey);

    // [Network]
    c.port = GI("Network", "UDPPort", c.port);
    // [General]
    c.autoEnable   = GI("General", "AutoEnable", c.autoEnable);
    c.diagnostics  = GI("General", "Diagnostics", c.diagnostics);
    // [Hotkeys]
    c.hotkeysEnabled = GI("Hotkeys", "Enabled", c.hotkeysEnabled);
    c.toggleKey      = GK("Hotkeys", "ToggleKey", c.toggleKey);
    c.recenterKey    = GK("Hotkeys", "RecenterKey", c.recenterKey);
    // [Test]
    c.wobbleTest = GI("Test", "WobbleTest", c.wobbleTest);
    c.wobbleMode = GI("Test", "WobbleMode", c.wobbleMode);
    c.wobbleDeg  = GF("Test", "WobbleDeg", c.wobbleDeg);
    c.wobbleLean = GF("Test", "WobbleLean", c.wobbleLean);
    c.wobbleHz   = GF("Test", "WobbleHz", c.wobbleHz);
    // [Rotation]
    c.yawSens    = GF("Rotation", "YawSensitivity", c.yawSens);
    c.pitchSens  = GF("Rotation", "PitchSensitivity", c.pitchSens);
    c.rollSens   = GF("Rotation", "RollSensitivity", c.rollSens);
    c.globalScale= GF("Rotation", "GlobalScale", c.globalScale);
    c.limitDeg   = GF("Rotation", "LimitDeg", c.limitDeg);
    c.invYaw     = GI("Rotation", "InvertYaw", c.invYaw);
    c.invPitch   = GI("Rotation", "InvertPitch", c.invPitch);
    c.invRoll    = GI("Rotation", "InvertRoll", c.invRoll);
    // [Camera]
    c.camRotEnabled  = GI("Camera", "RotationEnabled", c.camRotEnabled);
    c.camPosEnabled  = GI("Camera", "PositionEnabled", c.camPosEnabled);
    c.matrixTranspose= GI("Camera", "MatrixTranspose", c.matrixTranspose);
    c.camRotScale    = GF("Camera", "RotationScale", c.camRotScale);
    c.matrixOffset   = GH("Camera", "MatrixOffset", c.matrixOffset);
    // [Position]
    c.leanScale   = GF("Position", "LeanScale", c.leanScale);
    c.leanScaleX  = GF("Position", "LeanScaleX", c.leanScaleX);
    c.leanScaleY  = GF("Position", "LeanScaleY", c.leanScaleY);
    c.leanScaleZ  = GF("Position", "LeanScaleZ", c.leanScaleZ);
    c.leanLimit   = GF("Position", "LeanLimit", c.leanLimit);
    c.invX        = GI("Position", "InvertX", c.invX);
    c.invY        = GI("Position", "InvertY", c.invY);
    c.invZ        = GI("Position", "InvertZ", c.invZ);
    c.swapYZ      = GI("Position", "SwapYZ", c.swapYZ);
    c.worldUnitsPerMetre = GF("Position", "WorldUnitsPerMetre", c.worldUnitsPerMetre);
    c.softLimit   = GI("Position", "SoftLimit", c.softLimit);
    // [Filter]
    c.smoothMode = GI("Filter", "SmoothMode", c.smoothMode);
    c.smoothHz   = GF("Filter", "SmoothHz", c.smoothHz);
    c.smoothEMA  = GF("Filter", "SmoothEMA", c.smoothEMA);

    // Log key codes at startup
    Log("[3D] keys: mod=0x%X | sep 0x%X/0x%X | conv 0x%X/0x%X | mode 0x%X | swap 0x%X | fov 0x%X/0x%X | z 0x%X/0x%X",
        c.adjustModifier, c.separationDownKey, c.separationUpKey,
        c.convergenceDownKey, c.convergenceUpKey, c.modeCycleKey, c.swapEyesKey,
        c.fovDownKey, c.fovUpKey, c.zNearKey, c.zFarKey);
    Log("[3D] config: stereo=%d mode=%d sep=%.1fmm conv=%.2f WUPM=%.1f swapEyes=%d",
        c.stereoEnabled, c.outputMode, c.separationMM, c.convergenceDistance, c.worldUnitsPerMetre, c.swapEyes);
    Log("config: port=%d rotScale=%.2f lean %.2f/%.2f/%.2f lim%.2f matOff=0x%X fov(en%d @0x%X) transpose=%d",
        c.port, c.camRotScale, c.leanScaleX, c.leanScaleY, c.leanScaleZ, c.leanLimit,
        c.matrixOffset, c.fovEnabled, c.fovOffset, c.matrixTranspose);
}

void ConfigSaveValue(const char* section, const char* key, const char* value) {
    WritePrivateProfileStringA(section, key, value, ConfigIniPath().c_str());
}

void ConfigSaveFloat(const char* section, const char* key, float v) {
    char b[64]; snprintf(b, 64, "%.6f", v);
    ConfigSaveValue(section, key, b);
}

void ConfigSaveInt(const char* section, const char* key, int v) {
    char b[64]; snprintf(b, 64, "%d", v);
    ConfigSaveValue(section, key, b);
}

// ---- live hotkey processing ----
static KRep kSepD, kSepU, kConvD, kConvU, kFovD, kFovU, kZNear, kZFar, kMode, kSwap;

void ConfigProcessHotkeys() {
    if (!g_cfg.hotkeysEnabled) return;
    Config& c = g_cfg;

    // Tracking toggle/recenter (existing)
    static bool tp = false, rp = false;
    bool t = (GetAsyncKeyState(c.toggleKey) & 0x8000) != 0;
    bool r = (GetAsyncKeyState(c.recenterKey) & 0x8000) != 0;
    if (t && !tp) g_tog = true;
    if (r && !rp) g_rec = true;
    tp = t; rp = r;

    // Check modifier
    bool adj = c.adjustModifier ? (GetAsyncKeyState(c.adjustModifier) & 0x8000) != 0 : true;
    bool rev = c.reverseModifier ? (GetAsyncKeyState(c.reverseModifier) & 0x8000) != 0 : false;

    // Persist on modifier release (write back all live values)
    // MUST be BEFORE the `if (!adj) return;` check, otherwise the save
    // never executes when the modifier is released!
    static bool adjWasDown = false;
    if (adjWasDown && !adj) {
        ConfigSaveFloat("Stereo3D", "SeparationMM", c.separationMM);
        ConfigSaveFloat("Stereo3D", "ConvergenceDistance", c.convergenceDistance);
        ConfigSaveFloat("FOV", "Scale", c.fovScale);
        ConfigSaveFloat("ZAxis", "Offset", c.zOffset);
        Log("[3D] Config saved to INI");
    }
    adjWasDown = adj;

    if (!adj) return;

    // Separation
    if (kSepD.Fire((GetAsyncKeyState(c.separationDownKey) & 0x8000) != 0)) {
        float step = c.separationStep * (rev ? 1 : -1);
        c.separationMM += step;
        if (c.separationMM < 0) c.separationMM = 0;
        Log("[3D] SeparationMM = %.1f", c.separationMM);
    }
    if (kSepU.Fire((GetAsyncKeyState(c.separationUpKey) & 0x8000) != 0)) {
        float step = c.separationStep * (rev ? -1 : 1);
        c.separationMM += step;
        if (c.separationMM < 0) c.separationMM = 0;
        Log("[3D] SeparationMM = %.1f", c.separationMM);
    }

    // Convergence (Ctrl+F5/F6 adjusts ConvergenceDistance - smooth multiplicative)
    // ConvergenceDistance: multiplicative step (GBFR-style, 3% per tap)
    // IMPORTANT: convergenceStep MUST be 1.0 (not 3.0) to match GBFR's 3% rate.
    // Higher values cause exponential explosion when key is held.
    if (kConvD.Fire((GetAsyncKeyState(c.convergenceDownKey) & 0x8000) != 0)) {
        c.convergenceDistance = (c.convergenceDistance / (1.0f + 0.03f * c.convergenceStep));
        if (c.convergenceDistance < 0.5f) c.convergenceDistance = 0.5f;
        Log("[3D] ConvergenceDistance = %.2f", c.convergenceDistance);
    }
    if (kConvU.Fire((GetAsyncKeyState(c.convergenceUpKey) & 0x8000) != 0)) {
        c.convergenceDistance = (c.convergenceDistance * (1.0f + 0.03f * c.convergenceStep));
        Log("[3D] ConvergenceDistance = %.2f", c.convergenceDistance);
    }

    // FOV scale
    if (kFovD.Fire((GetAsyncKeyState(c.fovDownKey) & 0x8000) != 0)) {
        c.fovScale -= c.fovStep * (rev ? -1.f : 1.f);
        if (c.fovScale < 0.1f) c.fovScale = 0.1f;
        Log("[3D] FOV Scale = %.3f", c.fovScale);
    }
    if (kFovU.Fire((GetAsyncKeyState(c.fovUpKey) & 0x8000) != 0)) {
        c.fovScale += c.fovStep * (rev ? -1.f : 1.f);
        if (c.fovScale < 0.1f) c.fovScale = 0.1f;
        Log("[3D] FOV Scale = %.3f", c.fovScale);
    }

    // Z-dolly
    if (kZNear.Fire((GetAsyncKeyState(c.zNearKey) & 0x8000) != 0)) {
        c.zOffset += c.zStep * (rev ? -1.f : 1.f);
        Log("[3D] Z Offset = %.2f m", c.zOffset);
    }
    if (kZFar.Fire((GetAsyncKeyState(c.zFarKey) & 0x8000) != 0)) {
        c.zOffset -= c.zStep * (rev ? -1.f : 1.f);
        Log("[3D] Z Offset = %.2f m", c.zOffset);
    }

    // Output mode cycle
    if (kMode.Fire((GetAsyncKeyState(c.modeCycleKey) & 0x8000) != 0)) {
        c.outputMode = (c.outputMode + 1) % 7;
        ConfigSaveInt("Stereo3D", "OutputMode", c.outputMode);
        Log("[3D] OutputMode = %d", c.outputMode);
    }

    // Swap eyes toggle
    if (kSwap.Fire((GetAsyncKeyState(c.swapEyesKey) & 0x8000) != 0)) {
        c.swapEyes = c.swapEyes ? 0 : 1;
        ConfigSaveInt("Stereo3D", "SwapEyes", c.swapEyes);
        Log("[3D] SwapEyes = %d", c.swapEyes);
    }

    // (Config save is handled above, before the `if (!adj) return;` check)
}
