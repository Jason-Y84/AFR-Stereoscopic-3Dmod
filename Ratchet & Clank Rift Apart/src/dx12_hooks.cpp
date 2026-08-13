// ============================================================================
//  Rift Apart 3D+6DOF - DX12 Hooks (Present + ECL via dummy-object vtable patching)
//
//  STRATEGY (from RDR2-3D-6DOF): D3D12/DXGI COM objects share a single vtable
//  across ALL instances of the same type. We create temporary dummy objects
//  (device + queue + swapchain), patch their vtable slots for Present (8) and
//  ExecuteCommandLists (10), then release the dummies. The patch persists
//  because the vtable is shared - when the game creates its real swapchain and
//  queue, they use the SAME vtable and our hooks are already in place.
//
//  Then in the hooks themselves, we capture the REAL swapchain/device/queue
//  on first call.
// ============================================================================
#define INITGUID
#include <initguid.h>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstring>
#include <atomic>

#include "logger.h"
#include "config.h"
#include "compositor.h"
#include "camera_hook.h"
#include "afr_engine.h"

using namespace P5HT;

// ---- Captured objects (set on first hook call) ----
static IDXGISwapChain3*    g_sc    = nullptr;
static ID3D12Device*        g_dev12 = nullptr;
static ID3D12CommandQueue*  g_queue = nullptr;
static UINT                 g_bbW = 0, g_bbH = 0;
static DXGI_FORMAT          g_bbFmt = DXGI_FORMAT_UNKNOWN;

// ---- Original function pointers (set when vtable is patched) ----
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain3*, UINT, UINT);
typedef void     (STDMETHODCALLTYPE *PFN_ECL)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static PFN_Present g_realPresent = nullptr;
static PFN_ECL     g_realEcl = nullptr;

// ---- State ----
static bool g_hooksInstalled = false;

// ---- AFR eye state ----
static std::atomic<int> g_frameEye{0};     // eye for current frame (set by Present)

// Frame tag: incremented on each new Present frame (bbIdx change).
// Used by camera hook to detect frame boundaries and enforce ONE CAMERA
// PER FRAME rule (primary camera only, skipping UI/aux cameras).
// Definition here; camera_hook.cpp references via extern.
std::atomic<uint64_t> g_frameTag{0};

void Stereo_NoteCameraWrite() {}

// ---- Vtable patching helper ----
static bool VtblHook(void** vtbl, int idx, void* hook, void** orig) {
    DWORD op;
    if (!VirtualProtect(&vtbl[idx], sizeof(void*), PAGE_READWRITE, &op)) return false;
    *orig = vtbl[idx];
    vtbl[idx] = hook;
    VirtualProtect(&vtbl[idx], sizeof(void*), op, &op);
    return true;
}

static void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r,
                    D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    l->ResourceBarrier(1, &br);
}

// ---- ECL hook: count submissions per frame, detect frame boundaries ----
static void STDMETHODCALLTYPE HookECL(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    // Capture the first DIRECT queue we see
    if (!g_queue) {
        D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
        if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_queue = q;
            Log("[3D] DIRECT command queue captured: %p", (void*)q);
        }
    }

    g_realEcl(q, n, lists);
}

// ---- Present hook: composite once per frame using back buffer index ----
static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain3* sc, UINT sync, UINT flags) {
    // Capture swapchain on first call
    if (!g_sc) {
        g_sc = sc;
        if (SUCCEEDED(sc->GetDevice(IID_ID3D12Device, (void**)&g_dev12))) {
            DXGI_SWAP_CHAIN_DESC1 d{};
            if (SUCCEEDED(sc->GetDesc1(&d))) {
                g_bbW = d.Width; g_bbH = d.Height; g_bbFmt = d.Format;
            }
            Log("[3D] SwapChain captured: %ux%u fmt=%d", g_bbW, g_bbH, (int)g_bbFmt);
            Log("[3D] ID3D12Device captured: %p", (void*)g_dev12);
        }
    }

    // Get current back buffer index - this changes exactly once per frame
    UINT bbIdx = sc->GetCurrentBackBufferIndex();

    // Only composite when back buffer index changes (one composite per frame)
    static int lastBbIdx = -1;
    // DEBUG: log first 10 Present calls only
    static uint64_t presentCallCount = 0;
    bool newFrame = ((int)bbIdx != lastBbIdx);
    presentCallCount++;
    if (presentCallCount <= 10) {
        Log("[3D][PRESENT] call#%llu bbIdx=%u lastBbIdx=%d newFrame=%d",
            (unsigned long long)presentCallCount, bbIdx, lastBbIdx, (int)newFrame);
    }

    if (newFrame) {
        lastBbIdx = (int)bbIdx;

        // Bump frame tag - this is the authoritative frame boundary signal
        // for the camera hook (ONE CAMERA PER FRAME detection).
        // The increment uses relaxed ordering because all we need is
        // uniqueness across frames (no data dependency on the value).
        g_frameTag.fetch_add(1, std::memory_order_relaxed);

        // SIMPLE APPROACH: Present hook reads the current eye (set by previous
        // frame's flip), composites the slot, then flips for next frame.
        // No queue, no flag - just read, composite, flip.
        extern std::atomic<uint64_t> g_camHookCounter;
        static uint64_t s_lastCounter = 0;
        uint64_t counter = g_camHookCounter.load(std::memory_order_relaxed);
        bool rawHookRan = (counter != s_lastCounter);

        // Decoupled mono decision vs eye flip:
        // Eye flip is unconditional (Afr_BeginFrame every frame) to maintain
        // stable AFR cadence. Mono mode uses sticky grace (180 initial,
        // 5 normal) to avoid oscillation during intermittent hook calls.
        static int s_monoGrace = 180;  // large initial grace for startup
        static bool s_stereoActive = false;
        if (rawHookRan) {
            s_stereoActive = true;
            s_monoGrace = 5;          // refill grace on any activity
        } else if (s_monoGrace > 0) {
            s_monoGrace--;           // tolerate intermittent misses
        } else {
            s_stereoActive = false;  // truly stopped → mono
        }
        bool camHookRan = s_stereoActive;  // used for forceMono only

        // Log when camera hook state changes (start/stop being called)
        static bool s_wasRunning = false;
        if (camHookRan != s_wasRunning) {
            Log("[3D] Camera hook %s (counter=%llu -> %llu, grace=%d)",
                camHookRan ? "STARTED" : "STOPPED",
                (unsigned long long)s_lastCounter,
                (unsigned long long)counter, s_monoGrace);
            s_wasRunning = camHookRan;
        }

        // Auto-reinstall: if camera hook hasn't run for 30+ frames, check if it
        // was removed. If still installed (jump bytes present), just wait - the
        // game will use it when it enters a 3D scene.
        // Reduced from 60 to 30 frames for faster recovery from startup
        // timing issues (occasional SBS→2D collapse on game launch).
        static int s_noCamFrames = 0;
        if (rawHookRan) {
            s_noCamFrames = 0;
        } else {
            s_noCamFrames++;
            if (s_noCamFrames == 30) {
                Log("[3D] Camera hook not running for 30 frames - checking...");
                if (CameraHook_Reinstall(false)) {
                    Log("[3D] Hook still installed - waiting for game to use it");
                } else {
                    Log("[3D] Hook was removed - reinstalled");
                }
                s_noCamFrames = 0;  // check again in 30 frames
            }
        }
        s_lastCounter = counter;

        // Force mono only after grace period fully expires.
        // During grace, keep stereo mode so slots keep updating alternately.
        extern std::atomic<int> g_forceMonoMode;
        g_forceMonoMode.store(camHookRan ? 0 : 1, std::memory_order_release);

        // Dual update flag: when camera hook didn't actually run this frame
        // (rawHookRan=false) but we're still in stereo mode (grace active),
        // update BOTH slots to prevent stale content causing black flash.
        // This covers scene transitions where camera hook temporarily stops.
        extern std::atomic<int> g_dualUpdate;
        g_dualUpdate.store(!rawHookRan ? 1 : 0, std::memory_order_release);

        // Read the current eye
        int eye = Afr_FrameEye();
        g_frameEye.store(eye, std::memory_order_release);

        // DEBUG: verify eye alternation (first 10 frames only)
        static int lastEye = -1;
        static int eyeLogCount = 0;
        if (eyeLogCount < 10) {
            Log("[3D][PRESENT] newFrame bbIdx=%u eye=%d eyeChanged=%d camHookRan=%d",
                bbIdx, eye, (eye != lastEye), (int)camHookRan);
            lastEye = eye;
            eyeLogCount++;
        }

        if (g_queue && g_cfg.stereoEnabled) {
            // Always call Compositor_Execute, even when outputMode=0 (mono).
            // The compositor handles mono mode internally (passthrough).
            // Skipping it entirely causes a crash during game initialization
            // because BuildPipeline() never runs on the render thread,
            // leaving D3D12 pipeline objects uninitialized.
            Compositor_Execute(sc, eye);
        }

        // Flip eye unconditionally every frame to maintain AFR cadence.
        Afr_BeginFrame();
    }

    return g_realPresent(sc, sync, flags);
}

// ---- Create dummy objects and patch their vtables ----
static bool HookViaDummies() {
    Log("[3D] Creating dummy D3D12 objects for vtable hooking...");

    IDXGIFactory4* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_IDXGIFactory4, (void**)&fac))) {
        Log("[3D] CreateDXGIFactory1 failed");
        return false;
    }

    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device, (void**)&dev))) {
        Log("[3D] D3D12CreateDevice failed");
        fac->Release();
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_ID3D12CommandQueue, (void**)&q))) {
        Log("[3D] CreateCommandQueue failed");
        dev->Release(); fac->Release();
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RiftApart3D_dummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = 64;
    scd.Height = 64;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = fac->CreateSwapChainForHwnd(q, hwnd, &scd, nullptr, nullptr, &sc1);

    bool ok = false;
    if (SUCCEEDED(hr) && sc1) {
        void** scVtbl = *(void***)sc1;
        void** qVtbl  = *(void***)q;

        ok  = VtblHook(scVtbl, 8, (void*)&HookPresent, (void**)&g_realPresent);
        ok &= VtblHook(qVtbl, 10, (void*)&HookECL, (void**)&g_realEcl);

        Log("[3D] Vtable hooks installed: Present=%p ECL=%p", (void*)g_realPresent, (void*)g_realEcl);
        sc1->Release();
    } else {
        Log("[3D] Dummy swapchain creation failed: 0x%08X", (unsigned)hr);
    }

    if (hwnd) DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    q->Release();
    dev->Release();
    fac->Release();

    return ok;
}

// ---- Main hook installation ----
bool Dx12Hooks_Install() {
    if (g_hooksInstalled) return true;

    if (!HookViaDummies()) {
        Log("[3D] Failed to install vtable hooks - stereo disabled, tracking still works");
        return false;
    }

    g_hooksInstalled = true;
    Log("[3D] DX12 hooks installed via dummy vtable patching - waiting for game's swapchain/queue...");
    return true;
}

// ---- Accessors ----
IDXGISwapChain3*    Dx12_GetSwapChain() { return g_sc; }
ID3D12Device*       Dx12_GetDevice()    { return g_dev12; }
ID3D12CommandQueue* Dx12_GetDirectQueue() { return g_queue; }
bool Dx12_IsReady() { return g_sc && g_dev12 && g_queue; }

void Dx12_RealExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    g_realEcl(q, n, lists);
}
int Dx12_GetFrameEye() { return g_frameEye.load(std::memory_order_acquire); }

void Dx12Hooks_Cleanup() {
    g_hooksInstalled = false;
}

