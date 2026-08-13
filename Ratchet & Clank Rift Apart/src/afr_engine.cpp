// ============================================================================
//  Rift Apart 3D+6DOF - AFR Engine implementation
//  Eye clock, eye-queue FIFO, warmup, idle guard, cadence, QPC timing
// ============================================================================
#include "afr_engine.h"
#include "config.h"
#include "logger.h"
#include <algorithm>

using namespace P5HT;

// ---- QPC timing (§15.3) ----
double QpcNowMs() {
    static LARGE_INTEGER freq = {{0}};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart * 1000.0;
}

// ---- Eye clock (§13c) ----
// Latch and flip the eye once per displayed frame (PresentCount edge)
static std::atomic<unsigned> s_lastPresentCount{~0u};
static std::atomic<int> s_eye{0};

int Afr_FrameEye() {
    // This is called from the camera cave (game thread).
    // The actual flip is driven by Afr_BeginFrame() called from Present.
    // Between BeginFrame calls, the eye is stable.
    return s_eye.load(std::memory_order_relaxed);
}

void Afr_BeginFrame() {
    // Flip the eye for the next displayed frame
    int prev = s_eye.fetch_xor(1, std::memory_order_acq_rel);
    (void)prev;
}

// ---- Eye-queue FIFO (§13) ----
// SPSC ring buffer: push at ECL, pop at Present
static const int EYE_Q_CAP = 16;
static std::atomic<int> s_eyeQBuf[EYE_Q_CAP];
static std::atomic<int> s_eyeQW{0};  // write index (ECL thread)
static std::atomic<int> s_eyeQR{0};  // read index (Present thread)

void Afr_ReportCameraEye(int eye) {
    int w = s_eyeQW.load(std::memory_order_relaxed);
    s_eyeQBuf[w % EYE_Q_CAP].store(eye, std::memory_order_release);
    s_eyeQW.store(w + 1, std::memory_order_release);
}

int Afr_PopFrameEye() {
    int r = s_eyeQR.load(std::memory_order_relaxed);
    int w = s_eyeQW.load(std::memory_order_acquire);
    // Drain excess entries: if queue has more than 2 entries, jump to the
    // second-to-last (keep the latest push for this frame, discard old ones).
    // This prevents queue accumulation when camera hook is called multiple
    // times per frame (shadow/reflection passes for near objects).
    if ((w - r) > 2) {
        r = w - 2;  // take the newest, never drift
        s_eyeQR.store(r, std::memory_order_release);
    }
    if (r >= w) return s_eye.load(std::memory_order_relaxed);  // no entry -> use current eye
    int eye = s_eyeQBuf[r % EYE_Q_CAP].load(std::memory_order_acquire);
    s_eyeQR.store(r + 1, std::memory_order_release);
    return eye;
}

// ---- Warmup gate (§15.2) ----
static std::atomic<int> s_warmup{0};
static std::atomic<int> s_slotValid[2] = {0, 0};

bool Afr_IsWarmup() {
    int warm = s_warmup.load(std::memory_order_acquire);
    if (warm > 0) {
        // Both slots valid? settle
        if (s_slotValid[0].load() && s_slotValid[1].load()) {
            s_warmup.store(warm - 1, std::memory_order_release);
        }
        return true;
    }
    return false;
}

void Afr_ResetWarmup() {
    s_warmup.store(8, std::memory_order_release);
    s_slotValid[0].store(0, std::memory_order_release);
    s_slotValid[1].store(0, std::memory_order_release);
}

void Afr_MarkSlotValid(int eye) {
    s_slotValid[eye].store(1, std::memory_order_release);
}

// ---- Slot timestamps + fresh/stale (§15.5) ----
static double s_slotTime[2] = {0, 0};
static float s_slotYaw[2] = {0, 0};

void Afr_SetSlotTime(int eye, double ms) {
    s_slotTime[eye] = ms;
    Afr_MarkSlotValid(eye);
}

double Afr_SlotTimeMs(int slot) { return s_slotTime[slot]; }

int Afr_FreshSlot() {
    // Timestamp-based: unbiased selection (§15.5)
    return (s_slotTime[0] >= s_slotTime[1]) ? 0 : 1;
}

int Afr_StaleSlot() { return Afr_FreshSlot() ^ 1; }

// ---- Viewpoint yaw per slot (for reprojection) ----
void Afr_SetSlotYaw(int eye, float yaw) { s_slotYaw[eye] = yaw; }

static float s_reprojEMA = 0.f;

float Afr_GetReprojShift(int slot) {
    if (!g_cfg.afrReproject) return 0.f;
    int fresh = Afr_FreshSlot();
    int stale = fresh ^ 1;
    if (slot != stale) return 0.f;  // only shift the stale eye

    float d = s_slotYaw[fresh] - s_slotYaw[stale];
    // Unwrap
    while (d > 180.f) d -= 360.f;
    while (d < -180.f) d += 360.f;

    float shift = d * g_cfg.afrReprojectGain * 0.98f;
    if (shift > g_cfg.afrReprojectClamp) shift = g_cfg.afrReprojectClamp;
    if (shift < -g_cfg.afrReprojectClamp) shift = -g_cfg.afrReprojectClamp;

    // Low-pass EMA (§15.6)
    s_reprojEMA += (shift - s_reprojEMA) * 0.25f;
    return s_reprojEMA;
}

// ---- Idle guard (§15.10) ----
static int s_idleFrames = 0;
static const int kIdlePassthrough = 20;  // ~20 frames (not time!)
static bool s_wasIdle = false;

bool Afr_IdleCheck(int wrote) {
    if (wrote) {
        s_idleFrames = 0;
        if (s_wasIdle) {
            s_wasIdle = false;
            Afr_ResetWarmup();  // re-warm on resume
        }
        return false;  // not idle
    }
    s_idleFrames++;
    if (s_idleFrames > kIdlePassthrough) {
        s_wasIdle = true;
        return true;  // idle: copy live BB into a slot, forceMono
    }
    return false;
}

bool Afr_IsIdle() { return s_wasIdle; }

// ---- Pipeline depth pin (§13b) ----
static int s_pipelineDepth = 1;
void Afr_SetPipelineDepth(int depth) { s_pipelineDepth = depth; }

// ---- Cadence log (§16) ----
static long long s_cadPresentCount = 0;
static long long s_cadCamWriteCount = 0;
static long long s_cadEyeFlipCount = 0;
static long long s_cadHeldCount = 0;
static double s_cadLastTime = 0;
static float s_cadFrameTimeMin = 999.f, s_cadFrameTimeMax = 0.f;
static double s_cadLastFrame = 0;
static int s_prevEye = -1;

void Afr_TickCadence() {
    s_cadPresentCount++;

    double now = QpcNowMs();
    if (s_cadLastFrame > 0) {
        float ft = (float)(now - s_cadLastFrame);
        if (ft < s_cadFrameTimeMin) s_cadFrameTimeMin = ft;
        if (ft > s_cadFrameTimeMax) s_cadFrameTimeMax = ft;
    }
    s_cadLastFrame = now;

    // Track eye flips
    int curEye = s_eye.load(std::memory_order_relaxed);
    if (curEye != s_prevEye) {
        s_cadEyeFlipCount++;
    } else {
        s_cadHeldCount++;
    }
    s_prevEye = curEye;

    // Log once per second
    if (s_cadLastTime == 0) s_cadLastTime = now;
    double elapsed = now - s_cadLastTime;
    if (elapsed >= 1000.0) {
        float sec = (float)(elapsed / 1000.0);
        if (g_cfg.diagnostics) {
            int qdepth = s_eyeQW.load() - s_eyeQR.load();
            if (qdepth < 0) qdepth = 0;
            Log("[3D][cadence] present/s=%.0f camWrite/s=%.0f frametime min=%.1f/max=%.1f/jitter=%.1f eyeflips/s=%.0f held/s=%.0f qdepth=%d",
                s_cadPresentCount / sec, s_cadCamWriteCount / sec,
                s_cadFrameTimeMin, s_cadFrameTimeMax,
                s_cadFrameTimeMax - s_cadFrameTimeMin,
                s_cadEyeFlipCount / sec, s_cadHeldCount / sec, qdepth);
        }
        s_cadPresentCount = 0;
        s_cadCamWriteCount = 0;
        s_cadEyeFlipCount = 0;
        s_cadHeldCount = 0;
        s_cadFrameTimeMin = 999.f;
        s_cadFrameTimeMax = 0.f;
        s_cadLastTime = now;
    }
}

void Afr_NoteCamWrite() {
    s_cadCamWriteCount++;
}

// ---- Full reset ----
void Afr_Reset() {
    s_warmup.store(8);
    s_slotValid[0].store(0);
    s_slotValid[1].store(0);
    s_idleFrames = 0;
    s_wasIdle = false;
    s_slotTime[0] = s_slotTime[1] = 0;
    s_slotYaw[0] = s_slotYaw[1] = 0;
    s_reprojEMA = 0.f;
    s_eyeQW.store(0);
    s_eyeQR.store(0);
    s_prevEye = -1;
}
