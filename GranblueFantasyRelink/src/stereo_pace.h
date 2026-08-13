// stereo_pace.h - SpecialK-grade frame pacing + present-jitter tracking (API-independent).
// Pace BEFORE the real present (flattens present-point frametime -> AFR's two eyes stay evenly
// spaced). Uses a HIGH-RESOLUTION waitable timer (~0.5ms) + a short spin, not Sleep (15-16ms).
#pragma once
#include <windows.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Hold each present to a steady 1/maxFps interval. Call right before the real Present/SwapBuffers.
static inline void StereoPaceToHz(int maxFps) {
    if (maxFps <= 0) return;
    static HANDLE timer = nullptr; static bool tried = false;
    if (!tried) { tried = true;
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    static LARGE_INTEGER freq{}, last{}; if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    double target = 1.0 / maxFps;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (last.QuadPart) {
        double elapsed = double(now.QuadPart - last.QuadPart) / freq.QuadPart;
        double remaining = target - elapsed;
        if (remaining > 0.0) {
            if (timer && remaining > 0.001) {                    // coarse wait to ~0.5ms via HR timer
                LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((remaining - 0.0005) * 1e7); // rel, 100ns
                if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                    WaitForSingleObject(timer, (DWORD)(remaining * 1000.0) + 2); // OS-wait the bulk (low CPU)
            }
            do { QueryPerformanceCounter(&now);                   // spin the last bit for precision
                 elapsed = double(now.QuadPart - last.QuadPart) / freq.QuadPart; } while (elapsed < target);
        }
    }
    QueryPerformanceCounter(&last);
}

// Track inter-present interval; returns current interval ms and updates min/max accumulators.
struct PresentJitter { double lastMs=0, minMs=1e9, maxMs=0; uint64_t n=0;
    double tick() {
        LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
        double now = 1000.0 * double(t.QuadPart) / double(f.QuadPart);
        double dt = lastMs ? now - lastMs : 0.0; lastMs = now;
        if (dt > 0) { if (dt < minMs) minMs = dt; if (dt > maxMs) maxMs = dt; n++; }
        return dt;
    }
    void reset() { minMs = 1e9; maxMs = 0; n = 0; }
};
