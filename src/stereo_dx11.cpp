// stereo_dx11.cpp - DX11 present-time AFR stereo for GBFR
#include <winsock2.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstring>
#include <atomic>
#include "stereo_dx11.h"
#include "stereo_config.h"
#include "shaders_stereo3d.h"
#include "logger.h"

namespace Gbfr3D {
using P5HT::Log;

// ------------------------------------------------------------------ state
static StereoCfg           g_cfg;
static char                g_ini[MAX_PATH];
static HANDLE              g_single = nullptr;

static ID3D11Device*        g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain*      g_sc  = nullptr;
static UINT                 g_w = 0, g_h = 0;
static DXGI_FORMAT          g_fmt = DXGI_FORMAT_UNKNOWN;

static ID3D11VertexShader*  g_vs = nullptr;
static ID3D11PixelShader*   g_ps = nullptr;
static ID3D11SamplerState*  g_samp = nullptr;
static ID3D11Buffer*        g_cb = nullptr;
static ID3D11BlendState*    g_blend = nullptr;
static ID3D11RasterizerState* g_rast = nullptr;
static bool                 g_ready = false, g_pipeFailed = false;

// AFR persistent slots (0 = LEFT forever, 1 = RIGHT forever)
static ID3D11Texture2D*         g_slotTex[2] = {};
static ID3D11ShaderResourceView* g_slotSRV[2] = {};
static bool     g_slotValid[2] = {};
static double   g_slotTimeMs[2] = {0, 0};
static float    g_slotYaw[2] = {0, 0};
static bool     g_slotsBuilt = false;
static std::atomic<int> g_afrWarmup{0};
static int g_idleFrames = 0;
static bool g_wasIdle = false;
static const int kIdlePassthrough = 20;

// mode 6 shared surface
static ID3D11Texture2D*          g_viewerTex = nullptr;
static ID3D11RenderTargetView*   g_viewerRTV = nullptr;
static HANDLE                    g_katanga = nullptr;
static bool                      g_viewerBuilt = false;

// eye clock
static std::atomic<unsigned> g_eyeWr{0}, g_eyeRd{0};
static std::atomic<int>      g_eyeQ[16];
static std::atomic<uint32_t> g_camWrites{0};
static std::atomic<uint32_t> g_presents{0};
static std::atomic<int>      g_viewYawMilli{0};

// AFR shader extras
static float g_afrShiftL = 0, g_afrShiftR = 0, g_afrForceMono = 0, g_afrFreshLeft = 0;

// Forward declarations
static bool PopFrameEye(int& eyeOut);

// Release all D3D11 resources for rebuild on resolution change
static void ReleaseResources() {
    if (g_slotTex[0]) { g_slotTex[0]->Release(); g_slotTex[0] = nullptr; }
    if (g_slotTex[1]) { g_slotTex[1]->Release(); g_slotTex[1] = nullptr; }
    if (g_slotSRV[0]) { g_slotSRV[0]->Release(); g_slotSRV[0] = nullptr; }
    if (g_slotSRV[1]) { g_slotSRV[1]->Release(); g_slotSRV[1] = nullptr; }
    if (g_viewerTex) { g_viewerTex->Release(); g_viewerTex = nullptr; }
    if (g_viewerRTV) { g_viewerRTV->Release(); g_viewerRTV = nullptr; }
    if (g_katanga) { CloseHandle(g_katanga); g_katanga = nullptr; }
    g_slotsBuilt = false;
    g_viewerBuilt = false;
    g_slotValid[0] = g_slotValid[1] = false;
    g_afrWarmup.store(0, std::memory_order_relaxed);
    Log("[3D] Resources released for rebuild.");
}

// ------------------------------------------------------------------ helpers
static bool VtblHook(void** vtbl, int idx, void* hook, void** orig) {
    DWORD op;
    if (!VirtualProtect(&vtbl[idx], sizeof(void*), PAGE_READWRITE, &op)) return false;
    *orig = vtbl[idx];
    vtbl[idx] = hook;
    VirtualProtect(&vtbl[idx], sizeof(void*), op, &op);
    return true;
}

static ID3DBlob* Compile(const char* entry, const char* target) {
    ID3DBlob* c = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(kStereoHLSL, strlen(kStereoHLSL), "stereo3d", nullptr, nullptr, entry, target, 0, 0, &c, &err);
    if (FAILED(hr)) {
        if (err) { Log("[3D] shader %s: %.256s", entry, (char*)err->GetBufferPointer()); err->Release(); }
        return nullptr;
    }
    if (err) err->Release();
    return c;
}

static bool BuildPipeline() {
    if (g_pipeFailed) return false;
    ID3DBlob* vs = Compile("VSMain", "vs_4_0");
    ID3DBlob* ps = Compile("PSMain", "ps_4_0");
    if (!vs || !ps) { g_pipeFailed = true; return false; }
    if (FAILED(g_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) ||
        FAILED(g_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps))) {
        g_pipeFailed = true; return false;
    }
    vs->Release(); ps->Release();

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
    g_dev->CreateSamplerState(&sd, &g_samp);

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 64;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateBuffer(&cb, nullptr, &g_cb))) { g_pipeFailed = true; return false; }

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dev->CreateBlendState(&bd, &g_blend);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    g_dev->CreateRasterizerState(&rd, &g_rast);

    g_ready = true;
    Log("[3D] DX11 pipeline built (%ux%u).", g_w, g_h);
    return true;
}

static void EnsureSlots() {
    if (g_slotsBuilt || !g_dev) return;
    D3D11_TEXTURE2D_DESC t{};
    t.Width = g_w; t.Height = g_h; t.MipLevels = 1; t.ArraySize = 1;
    t.Format = g_fmt; t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < 2; i++) {
        if (FAILED(g_dev->CreateTexture2D(&t, nullptr, &g_slotTex[i]))) return;
        if (FAILED(g_dev->CreateShaderResourceView(g_slotTex[i], nullptr, &g_slotSRV[i]))) return;
    }
    g_slotsBuilt = true;
    g_afrWarmup.store(8, std::memory_order_relaxed);
    g_slotValid[0] = g_slotValid[1] = false;
    Log("[3D][AFR] DX11 slots built.");
}

static void EnsureViewer() {
    if (g_viewerBuilt || !g_dev || g_cfg.outputMode != 6) return;
    D3D11_TEXTURE2D_DESC t{};
    t.Width = g_w * 2; t.Height = g_h; t.MipLevels = 1; t.ArraySize = 1;
    t.Format = g_fmt; t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT;
    t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    t.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(g_dev->CreateTexture2D(&t, nullptr, &g_viewerTex))) {
        Log("[3D][VR] shared tex failed"); return;
    }
    g_dev->CreateRenderTargetView(g_viewerTex, nullptr, &g_viewerRTV);
    IDXGIResource* r = nullptr;
    HANDLE sh = nullptr;
    if (SUCCEEDED(g_viewerTex->QueryInterface(__uuidof(IDXGIResource), (void**)&r)) && r) {
        r->GetSharedHandle(&sh); r->Release();
    }
    g_katanga = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4, L"Local\\KatangaMappedFile");
    if (g_katanga) {
        void* pv = MapViewOfFile(g_katanga, FILE_MAP_WRITE, 0, 0, 4);
        if (pv) { *(UINT32*)pv = (UINT32)(UINT_PTR)sh; UnmapViewOfFile(pv); }
    }
    g_viewerBuilt = true;
    Log("[3D][VR] shared surface %ux%u published.", g_w * 2, g_h);
}

static void WriteCB(UINT outW, UINT outH) {
    float p[12] = { g_cfg.afrConv, (float)outW, (float)outH, (float)g_cfg.outputMode,
        g_cfg.swapEyes ? 1.f : 0.f, g_afrShiftL, g_afrShiftR, g_afrForceMono,
        g_afrFreshLeft, 0.f, 0.f, 0.f };
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, p, sizeof(p));
        g_ctx->Unmap(g_cb, 0);
    }
}

struct SavedState {
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* il = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11Buffer* cb0 = nullptr;
    ID3D11SamplerState* samp0 = nullptr;
    ID3D11ShaderResourceView* srv[2] = {};
    ID3D11BlendState* blend = nullptr;
    FLOAT bf[4]; UINT mask = 0;
    ID3D11RasterizerState* rs = nullptr;
    D3D11_VIEWPORT vp;
    UINT nvp = 1;
    ID3D11GeometryShader* gs = nullptr;
};

static void SaveState(SavedState& s) {
    g_ctx->OMGetRenderTargets(1, &s.rtv, &s.dsv);
    g_ctx->VSGetShader(&s.vs, nullptr, nullptr);
    g_ctx->PSGetShader(&s.ps, nullptr, nullptr);
    g_ctx->GSGetShader(&s.gs, nullptr, nullptr);
    g_ctx->IAGetInputLayout(&s.il);
    g_ctx->IAGetPrimitiveTopology(&s.topo);
    g_ctx->PSGetConstantBuffers(0, 1, &s.cb0);
    g_ctx->PSGetSamplers(0, 1, &s.samp0);
    g_ctx->PSGetShaderResources(0, 2, s.srv);
    g_ctx->OMGetBlendState(&s.blend, s.bf, &s.mask);
    g_ctx->RSGetState(&s.rs);
    s.nvp = 1;
    g_ctx->RSGetViewports(&s.nvp, &s.vp);
}

static void RestoreState(SavedState& s) {
    g_ctx->OMSetRenderTargets(1, &s.rtv, s.dsv);
    g_ctx->VSSetShader(s.vs, nullptr, 0);
    g_ctx->PSSetShader(s.ps, nullptr, 0);
    g_ctx->GSSetShader(s.gs, nullptr, 0);
    g_ctx->IASetInputLayout(s.il);
    g_ctx->IASetPrimitiveTopology(s.topo);
    g_ctx->PSSetConstantBuffers(0, 1, &s.cb0);
    g_ctx->PSSetSamplers(0, 1, &s.samp0);
    g_ctx->PSSetShaderResources(0, 2, s.srv);
    g_ctx->OMSetBlendState(s.blend, s.bf, s.mask);
    g_ctx->RSSetState(s.rs);
    if (s.nvp) g_ctx->RSSetViewports(1, &s.vp);
    if (s.rtv) s.rtv->Release();
    if (s.dsv) s.dsv->Release();
    if (s.vs) s.vs->Release();
    if (s.ps) s.ps->Release();
    if (s.gs) s.gs->Release();
    if (s.il) s.il->Release();
    if (s.cb0) s.cb0->Release();
    if (s.samp0) s.samp0->Release();
    if (s.srv[0]) s.srv[0]->Release();
    if (s.srv[1]) s.srv[1]->Release();
    if (s.blend) s.blend->Release();
    if (s.rs) s.rs->Release();
}

static void DrawFS(ID3D11RenderTargetView* rtv, ID3D11ShaderResourceView* s0, ID3D11ShaderResourceView* s1, UINT vw, UINT vh) {
    ID3D11ShaderResourceView* nul[2] = { nullptr, nullptr };
    g_ctx->PSSetShaderResources(0, 2, nul);
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{ 0, 0, (float)vw, (float)vh, 0, 1 };
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->PSSetSamplers(0, 1, &g_samp);
    ID3D11ShaderResourceView* srv[2] = { s0, s1 ? s1 : s0 };
    g_ctx->PSSetShaderResources(0, 2, srv);
    float bf[4] = { 0, 0, 0, 0 };
    g_ctx->OMSetBlendState(g_blend, bf, 0xFFFFFFFF);
    g_ctx->RSSetState(g_rast);
    g_ctx->Draw(3, 0);
    g_ctx->PSSetShaderResources(0, 2, nul);
}

// AFR composite
static double NowMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return 1000.0 * double(t.QuadPart) / double(f.QuadPart);
}

static double g_frameMsEMA = 11.1;

static void UpdateFrameMs() {
    static double last = 0;
    double n = NowMs();
    if (last > 0) { double dt = n - last; if (dt > 0.1 && dt < 200.0) g_frameMsEMA += 0.10 * (dt - g_frameMsEMA); }
    last = n;
}

static bool AfrComposite() {
    if (!g_ready) { if (!BuildPipeline()) return false; }
    EnsureSlots();
    if (!g_slotsBuilt) return false;
    if (g_cfg.outputMode == 6) EnsureViewer();

    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return false;
    ID3D11RenderTargetView* bbRTV = nullptr;
    if (FAILED(g_dev->CreateRenderTargetView(bb, nullptr, &bbRTV)) || !bbRTV) {
        bb->Release(); return false;
    }

    int eye = 0;
    bool wrote = PopFrameEye(eye);

    if (wrote) { g_idleFrames = 0; }
    else if (++g_idleFrames > kIdlePassthrough) {
        g_wasIdle = true;
        g_ctx->CopyResource(g_slotTex[0], bb);
        g_slotValid[0] = true;
        g_slotTimeMs[0] = NowMs();
        g_afrForceMono = 1.f;
        g_afrFreshLeft = 1.f;
        g_afrShiftL = g_afrShiftR = 0.f;
        SavedState ss; SaveState(ss);
        ID3D11ShaderResourceView* L = g_slotSRV[0], *R = g_slotSRV[0];
        if (g_cfg.outputMode == 6 && g_viewerRTV) {
            WriteCB(g_w * 2, g_h); DrawFS(g_viewerRTV, L, R, g_w * 2, g_h);
            WriteCB(g_w, g_h); DrawFS(bbRTV, L, R, g_w, g_h);
        } else {
            WriteCB(g_w, g_h); DrawFS(bbRTV, L, R, g_w, g_h);
        }
        RestoreState(ss);
        bbRTV->Release();
        bb->Release();
        return true;
    }
    if (g_wasIdle && wrote) {
        g_wasIdle = false;
        g_afrWarmup.store(8, std::memory_order_relaxed);
    }

    if (wrote) {
        g_ctx->CopyResource(g_slotTex[eye], bb);
        g_slotValid[eye] = true;
        g_slotTimeMs[eye] = NowMs();
        g_slotYaw[eye] = g_viewYawMilli.load(std::memory_order_relaxed) / 1000.f;
    }

    UpdateFrameMs();
    int fresh = (g_slotTimeMs[0] >= g_slotTimeMs[1]) ? 0 : 1;
    int stale = fresh ^ 1;
    g_afrShiftL = g_afrShiftR = g_afrForceMono = g_afrFreshLeft = 0.f;
    bool freshOnly = false;
    int warm = g_afrWarmup.load(std::memory_order_relaxed);

    if (warm > 0) {
        if (g_slotValid[0] && g_slotValid[1]) g_afrWarmup.store(warm - 1, std::memory_order_relaxed);
        if (g_slotValid[fresh]) { g_afrForceMono = 1.f; g_afrFreshLeft = (fresh == 0) ? 1.f : 0.f; freshOnly = true; }
    } else if (g_slotValid[fresh] && g_slotValid[stale]) {
        if (g_cfg.afrReproject) {
            float d = g_slotYaw[fresh] - g_slotYaw[stale];
            while (d > 180.f) d -= 360.f;
            while (d < -180.f) d += 360.f;
            float sh = d * g_cfg.afrReprojGain * 0.98f;
            if (sh > g_cfg.afrReprojClamp) sh = g_cfg.afrReprojClamp;
            if (sh < -g_cfg.afrReprojClamp) sh = -g_cfg.afrReprojClamp;
            static float s_reprojEMA = 0.f;
            s_reprojEMA += (sh - s_reprojEMA) * 0.25f;
            sh = s_reprojEMA;
            if (stale == 0) g_afrShiftL = sh; else g_afrShiftR = sh;
        }
    } else if (g_slotValid[fresh]) {
        g_afrForceMono = 1.f;
        g_afrFreshLeft = (fresh == 0) ? 1.f : 0.f;
        freshOnly = true;
    }

    bool can = g_slotValid[fresh] && (freshOnly || g_slotValid[stale]);
    if (can) {
        SavedState ss; SaveState(ss);
        ID3D11ShaderResourceView* L = g_slotSRV[0];
        ID3D11ShaderResourceView* R = g_slotSRV[1];
        if (freshOnly) { L = R = g_slotSRV[fresh]; }
        if (g_cfg.outputMode == 6 && g_viewerRTV) {
            WriteCB(g_w * 2, g_h); DrawFS(g_viewerRTV, L, R, g_w * 2, g_h);
            WriteCB(g_w, g_h);
            DrawFS(bbRTV, L, R, g_w, g_h);
        } else {
            WriteCB(g_w, g_h);
            DrawFS(bbRTV, L, R, g_w, g_h);
        }
        RestoreState(ss);
    }

    if (wrote) {
        // eye flip handled by camera side
    }
    bbRTV->Release();
    bb->Release();
    return true;
}

// Present hook
typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
static PresentFn g_realPresent = nullptr;

static void PaceFrame() {
    if (g_cfg.enabled) {
        if (g_cfg.maxFps > 0) {
            static LARGE_INTEGER freq = {0};
            if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
            static LARGE_INTEGER last = {0};
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double frameTime = 1000.0 / g_cfg.maxFps;
            if (last.QuadPart) {
                double elapsed = 1000.0 * double(now.QuadPart - last.QuadPart) / double(freq.QuadPart);
                if (elapsed < frameTime) {
                    Sleep((DWORD)(frameTime - elapsed));
                }
            }
            last = now;
        }
    }
}

static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    g_presents.fetch_add(1, std::memory_order_relaxed);
    if (!g_sc) {
        g_sc = sc;
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_dev)) && g_dev) {
            g_dev->GetImmediateContext(&g_ctx);
            DXGI_SWAP_CHAIN_DESC d{};
            if (SUCCEEDED(sc->GetDesc(&d))) {
                g_w = d.BufferDesc.Width;
                g_h = d.BufferDesc.Height;
                g_fmt = d.BufferDesc.Format;
            }
            if (g_cfg.maxFrameLatency > 0) {
                IDXGIDevice1* dxdev = nullptr;
                if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxdev)) && dxdev) {
                    dxdev->SetMaximumFrameLatency((UINT)g_cfg.maxFrameLatency);
                    dxdev->Release();
                    Log("[3D] D3D11 max frame latency set to %d", g_cfg.maxFrameLatency);
                }
            }
            Log("=============== GBFR STEREO BRING-UP (DX11) ===============");
            Log("[3D] build %s %s  swapchain %ux%u fmt=%d", __DATE__, __TIME__, g_w, g_h, (int)g_fmt);
            Log("[3D] AFR outputMode=%d sepUnits=%.3f afrReproject=%d",
                g_cfg.outputMode, g_cfg.sepUnits, g_cfg.afrReproject);
            Log("[3D] keys: mod=0x%02X | sep 0x%02X/0x%02X | conv 0x%02X/0x%02X | mode 0x%02X | swap 0x%02X",
                g_cfg.adjustMod, g_cfg.sepDownKey, g_cfg.sepUpKey, g_cfg.convDownKey, g_cfg.convUpKey,
                g_cfg.modeCycleKey, g_cfg.swapKey);
            Log("=====================================================");
        }
    }
    // Check for resolution change (e.g., user switches to 2K)
    if (g_sc && g_w > 0 && g_h > 0) {
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(g_sc->GetDesc(&d))) {
            UINT curW = d.BufferDesc.Width;
            UINT curH = d.BufferDesc.Height;
            if (curW != g_w || curH != g_h) {
                Log("[3D] Resolution changed: %ux%u -> %ux%u, rebuilding...", g_w, g_h, curW, curH);
                g_w = curW;
                g_h = curH;
                g_fmt = d.BufferDesc.Format;
                ReleaseResources();
            }
        }
    }
    if (g_ctx && g_cfg.enabled && g_cfg.outputMode != 0 && !g_pipeFailed) {
        Stereo_PresentEyeFallback();
        AfrComposite();
    }
    PaceFrame();
    return g_realPresent(sc, sync, flags);
}

// Hook install
static bool HookPresent() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"gbfr3d_dummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = 64;
    scd.BufferDesc.Height = 64;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &scd, &sc, &dev, &fl, &ctx);
    bool ok = false;
    if (SUCCEEDED(hr) && sc) {
        void** vt = *(void***)sc;
        ok = VtblHook(vt, 8, (void*)&hkPresent, (void**)&g_realPresent);
        Log("[3D] present hook %s (real=%p)", ok ? "installed" : "FAILED", (void*)g_realPresent);
    } else {
        Log("[3D] dummy swapchain failed hr=0x%08lX - stereo off", (unsigned long)hr);
    }
    if (sc) sc->Release();
    if (dev) dev->Release();
    if (ctx) ctx->Release();
    if (hwnd) DestroyWindow(hwnd);
    return ok;
}

// ---- public API ----
bool  Stereo_Init(const char* iniPath) {
    strncpy_s(g_ini, MAX_PATH, iniPath, _TRUNCATE);
    g_single = CreateMutexA(nullptr, TRUE, "Local\\GBFR-3D-SingleInstance");
    if (g_single && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(g_single); g_single = nullptr; return false; }
    StereoLoad(g_cfg, g_ini);
    Log("[3D] cfg enabled=%d mode=%d sepUnits=%.3f (AFR-only)", g_cfg.enabled, g_cfg.outputMode, g_cfg.sepUnits);
    if (!g_cfg.enabled) { Log("[3D] disabled by config. Set [Stereo3D] Enabled=1."); return true; }
    if (!HookPresent()) Log("[3D] hooks not installed - continuing head-tracking only.");
    return true;
}

void  Stereo_Shutdown() {
    if (g_katanga) CloseHandle(g_katanga);
    if (g_single) CloseHandle(g_single);
}

void  Stereo_PollHotkeys() {
    if (StereoPollHotkeys(g_cfg, g_ini))
        Log("[3D] mode=%d sepMM=%.1f (units %.3f) convDist=%.2f (hit %.4f) swap=%d",
            g_cfg.outputMode, g_cfg.separationMM, g_cfg.sepUnits, g_cfg.convergenceDistance, g_cfg.afrConv, g_cfg.swapEyes);
}

bool  Stereo_AfrActive() { return g_cfg.enabled && g_cfg.outputMode != 0; }

int   Stereo_CurrentEyeSign() {
    const int eye = Stereo_FrameEye();
    Stereo_ReportCameraEye(eye);
    return eye ? +1 : -1;
}

float Stereo_SepPerEyeUnits() { return g_cfg.sepUnits; }

float Stereo_ConvergenceDistance() { return g_cfg.convergenceDistance; }

float Stereo_WorldUnitsPerMetre() { return g_cfg.worldUnitsPerMetre; }

unsigned Stereo_FrameIndex() { return (unsigned)g_presents.load(std::memory_order_relaxed); }

int Stereo_FrameEye() {
    static std::atomic<unsigned> s_lastFrame{ 0xFFFFFFFFu };
    static std::atomic<int>      s_eye{ 0 };
    unsigned f = (unsigned)g_presents.load(std::memory_order_relaxed);
    unsigned prev = s_lastFrame.load(std::memory_order_relaxed);
    if (f != prev && s_lastFrame.compare_exchange_strong(prev, f, std::memory_order_acq_rel))
        s_eye.fetch_xor(1, std::memory_order_acq_rel);
    return s_eye.load(std::memory_order_acquire);
}

void Stereo_ReportCameraEye(int eye) {
    unsigned w = g_eyeWr.fetch_add(1, std::memory_order_acq_rel);
    g_eyeQ[w & 15].store(eye, std::memory_order_release);
}

static bool PopFrameEye(int& eyeOut) {
    unsigned r = g_eyeRd.load(std::memory_order_relaxed);
    const unsigned w = g_eyeWr.load(std::memory_order_acquire);
    if (r == w) return false;
    if ((unsigned)(w - r) > 8u) r = w - 2u;
    eyeOut = g_eyeQ[r & 15].load(std::memory_order_acquire);
    g_eyeRd.store(r + 1u, std::memory_order_release);
    return true;
}

void  Stereo_NoteCameraWrite() {
    g_camWrites.fetch_add(1, std::memory_order_relaxed);
}

// Present-based fallback eye clock: when the camera hook is not firing
// (g_camWrites has not increased for several frames), derive the eye from the
// Present counter so AFR still works.  This keeps 3D functional even when the
// AOB pattern does not match the current game build.
static std::atomic<uint32_t> g_lastCamCheckPresent{0};
static std::atomic<int>      g_fallbackEye{0};
static std::atomic<int>      g_fallbackActive{0};

void  Stereo_PresentEyeFallback() {
    if (!g_cfg.enabled || g_cfg.outputMode == 0) return;
    uint32_t camWritesNow = g_camWrites.load(std::memory_order_relaxed);
    uint32_t presentsNow  = (uint32_t)g_presents.load(std::memory_order_relaxed);
    uint32_t lastCheck    = g_lastCamCheckPresent.load(std::memory_order_relaxed);

    // Every 10 presents, check if camera writes are keeping up
    if (presentsNow - lastCheck >= 10) {
        g_lastCamCheckPresent.store(presentsNow, std::memory_order_relaxed);
        static uint32_t s_lastCamWrites = 0;
        uint32_t delta = camWritesNow - s_lastCamWrites;
        s_lastCamWrites = camWritesNow;
        if (delta == 0 && presentsNow > 30) {
            if (g_fallbackActive.exchange(1) == 0)
                Log("[3D] camera write silent - activating Present-based eye fallback");
        } else if (delta > 0 && g_fallbackActive.exchange(0) == 1) {
            Log("[3D] camera writes detected - disabling Present fallback");
        }
    }

    if (g_fallbackActive.load(std::memory_order_relaxed)) {
        int eye = Stereo_FrameEye();
        Stereo_ReportCameraEye(eye);
    }
}

} // namespace Gbfr3D
