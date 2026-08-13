// ============================================================================
//  Rift Apart 3D+6DOF - Compositor (DX12 pipeline, AFR eye slots, composite draw)
//  Based on RDR2-3D-6DOF stereo_dx12.cpp structure.
//
//  Manages: PSO, root signature, descriptor heap, 3-allocator ring, fence,
//  persistent eye slot textures (LEFT=t0, RIGHT=t1), composite full-screen
//  triangle pass, and pipeline depth pinning.
// ============================================================================
#define INITGUID
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <atomic>

#include "compositor.h"
#include "config.h"
#include "logger.h"
#include "afr_engine.h"
#include "dx12_hooks.h"
#include "stereo_math.h"

using namespace P5HT;

// ---- Shader source (runtime-compiled via D3DCompile) ----
static const char* kStereoHLSL = R"HLSL(
Texture2D    texL   : register(t0);   // LEFT slot
Texture2D    texR   : register(t1);   // RIGHT slot
SamplerState sPoint : register(s0);   // POINT, MIRROR address

cbuffer Params : register(b0) {
    float gConv;
    float gOutW;
    float gOutH;
    float gMode;
    float gSwapEyes;
    float gShiftL;
    float gShiftR;
    float gForceMono;
    float gFreshLeft;
    float3 _pad;
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    int mode = (int)(gMode + 0.5);
    float2 uv = i.uv;
    float  px = i.pos.x, py = i.pos.y;

    if (mode == 0) {
        float3 c = texL.SampleLevel(sPoint, uv, 0).rgb;
        return float4(c, 1);
    }

    float  eyeSign;
    float2 srcUV;
    if (mode == 1 || mode == 6) {
        bool left = uv.x < 0.5; eyeSign = left ? -1.0 : 1.0;
        srcUV = float2(frac(uv.x * 2.0), uv.y);
    } else if (mode == 2) {
        bool left = uv.y < 0.5; eyeSign = left ? -1.0 : 1.0;
        srcUV = float2(uv.x, frac(uv.y * 2.0));
    } else if (mode == 3) {
        bool left = (fmod(floor(py), 2.0) < 0.5); eyeSign = left ? -1.0 : 1.0; srcUV = uv;
    } else if (mode == 4) {
        bool left = (fmod(floor(px), 2.0) < 0.5); eyeSign = left ? -1.0 : 1.0; srcUV = uv;
    } else {
        bool left = (fmod(floor(px) + floor(py), 2.0) < 0.5); eyeSign = left ? -1.0 : 1.0; srcUV = uv;
    }

    if (gForceMono > 0.5) {
        float3 c = (gFreshLeft > 0.5) ? texL.SampleLevel(sPoint, srcUV, 0).rgb
                                      : texR.SampleLevel(sPoint, srcUV, 0).rgb;
        return float4(c, 1);
    }

    if (gSwapEyes > 0.5) eyeSign = -eyeSign;
    float extra = (eyeSign < 0.0) ? gShiftL : gShiftR;
    float2 s = float2(srcUV.x - eyeSign * gConv + extra, srcUV.y);
    float3 c = (eyeSign < 0.0) ? texL.SampleLevel(sPoint, s, 0).rgb
                               : texR.SampleLevel(sPoint, s, 0).rgb;

    return float4(c, 1);
}
)HLSL";

// ---- State ----
static bool g_ready = false;
static bool g_pipeFailed = false;

// Pipeline objects
static const int kRing = 3;
static ID3D12CommandAllocator*       g_alloc[kRing] = {};
static ID3D12GraphicsCommandList*     g_list = nullptr;
static ID3D12Fence*                   g_fence = nullptr;
static UINT64                         g_fenceVal = 0, g_ringFence[kRing] = {};
static HANDLE                         g_fenceEvt = nullptr;
static int                            g_ringIdx = 0;

static ID3D12RootSignature*           g_root = nullptr;
static ID3D12PipelineState*           g_pso = nullptr;
static ID3D12DescriptorHeap*          g_srvHeap = nullptr;
static ID3D12DescriptorHeap*          g_rtvHeap = nullptr;
static UINT                           g_srvInc = 0, g_rtvInc = 0;
static ID3D12Resource*                g_paramCB = nullptr;
static UINT8*                         g_paramPtr = nullptr;

// AFR slots (persistent: 0=LEFT, 1=RIGHT)
static ID3D12Resource*                g_slot[2] = {};
static bool                           g_slotValid[2] = {};
static bool                           g_slotsBuilt = false;
static uint64_t                      g_presentFrame = 0;

// ---- Helpers ----
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

// ---- Shader compilation (runtime) ----
static ID3DBlob* CompileShader(const char* src, const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), "stereo3d", nullptr, nullptr,
                            entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        if (err) Log("[3D] shader %s error: %.256s", entry, (char*)err->GetBufferPointer());
        if (err) err->Release();
        return nullptr;
    }
    if (err) err->Release();
    return code;
}

// ---- Build pipeline (PSO, root sig, heaps, fence) ----
static bool BuildPipeline() {
    if (g_ready) return true;
    if (g_pipeFailed) return false;
    ID3D12Device* dev = Dx12_GetDevice();
    ID3D12CommandQueue* queue = Dx12_GetDirectQueue();
    IDXGISwapChain3* sc = Dx12_GetSwapChain();
    if (!dev || !queue || !sc) return false;

    UINT bbW = 0, bbH = 0;
    DXGI_FORMAT bbFmt = DXGI_FORMAT_UNKNOWN;
    DXGI_SWAP_CHAIN_DESC1 d{};
    if (SUCCEEDED(sc->GetDesc1(&d))) {
        bbW = d.Width; bbH = d.Height; bbFmt = d.Format;
    }

    // Command allocators + list
    for (int i = 0; i < kRing; ++i) {
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_ID3D12CommandAllocator, (void**)&g_alloc[i]))) { g_pipeFailed = true; return false; }
    }
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr,
            IID_ID3D12GraphicsCommandList, (void**)&g_list))) { g_pipeFailed = true; return false; }
    g_list->Close();

    // Fence
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, (void**)&g_fence))) {
        g_pipeFailed = true; return false;
    }
    g_fenceEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Root signature: CBV(b0) + SRV table(t0,t1) + static POINT sampler(s0)
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[0].Descriptor.ShaderRegister = 0;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &range;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = rp;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* rsBlob = nullptr;
    ID3DBlob* rsErr = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr))) {
        g_pipeFailed = true; return false;
    }
    if (FAILED(dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
            IID_ID3D12RootSignature, (void**)&g_root))) { g_pipeFailed = true; return false; }
    rsBlob->Release();
    if (rsErr) rsErr->Release();

    // Compile shaders at runtime (SM 5.1 for SV_VertexID support)
    ID3DBlob* vs = CompileShader(kStereoHLSL, "VSMain", "vs_5_1");
    ID3DBlob* ps = CompileShader(kStereoHLSL, "PSMain", "ps_5_1");
    if (!vs || !ps) {
        Log("[3D] Shader compilation failed");
        g_pipeFailed = true;
        if (vs) vs->Release();
        if (ps) ps->Release();
        return false;
    }

    // PSO
    // Use SV_VertexID for fullscreen triangle generation - no input layout needed
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_root;
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = bbFmt;
    pd.SampleDesc.Count = 1;

    if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_ID3D12PipelineState, (void**)&g_pso))) {
        g_pipeFailed = true;
        vs->Release(); ps->Release();
        return false;
    }
    vs->Release(); ps->Release();

    // Descriptor heaps
    D3D12_DESCRIPTOR_HEAP_DESC sh{};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = 4;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&sh, IID_ID3D12DescriptorHeap, (void**)&g_srvHeap))) {
        g_pipeFailed = true; return false;
    }
    g_srvInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC rh{};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = 4;
    if (FAILED(dev->CreateDescriptorHeap(&rh, IID_ID3D12DescriptorHeap, (void**)&g_rtvHeap))) {
        g_pipeFailed = true; return false;
    }
    g_rtvInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Param CB (256-byte upload buffer)
    D3D12_HEAP_PROPERTIES up{};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC cb{};
    cb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cb.Width = 256;
    cb.Height = 1;
    cb.DepthOrArraySize = 1;
    cb.MipLevels = 1;
    cb.Format = DXGI_FORMAT_UNKNOWN;
    cb.SampleDesc.Count = 1;
    cb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &cb,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_ID3D12Resource, (void**)&g_paramCB))) {
        g_pipeFailed = true; return false;
    }
    D3D12_RANGE nr{0, 256};
    HRESULT mapHr = g_paramCB->Map(0, &nr, (void**)&g_paramPtr);
    if (SUCCEEDED(mapHr) && g_paramPtr) {
        // Zero-fill initially
        memset(g_paramPtr, 0, 256);
        // Use explicit fence to handle cache coherency (no FlushCache needed on MSVC)
        Log("[3D] cbuffer mapped at %p", (void*)g_paramPtr);
    } else {
        Log("[3D] WARNING: cbuffer Map failed: 0x%08X", mapHr);
    }

    Log("[3D] Pipeline built (ring=%d, %ux%u)", kRing, bbW, bbH);
    g_ready = true;
    return true;
}

// ---- Create eye slot textures ----
static void EnsureSlots() {
    if (g_slotsBuilt || !g_ready) return;
    ID3D12Device* dev = Dx12_GetDevice();
    if (!dev || !g_srvHeap) return;

    IDXGISwapChain3* sc = Dx12_GetSwapChain();
    DXGI_SWAP_CHAIN_DESC1 d{};
    if (FAILED(sc->GetDesc1(&d))) return;
    UINT bbW = d.Width, bbH = d.Height;
    DXGI_FORMAT bbFmt = d.Format;

    D3D12_HEAP_PROPERTIES dp{};
    dp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC t{};
    t.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    t.Width = bbW;
    t.Height = bbH;
    t.DepthOrArraySize = 1;
    t.MipLevels = 1;
    t.Format = bbFmt;
    t.SampleDesc.Count = 1;

    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Format = bbFmt;
    sv.Texture2D.MipLevels = 1;

    for (int i = 0; i < 2; ++i) {
        if (FAILED(dev->CreateCommittedResource(&dp, D3D12_HEAP_FLAG_NONE, &t,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_ID3D12Resource, (void**)&g_slot[i]))) return;
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (size_t)i * g_srvInc;
        dev->CreateShaderResourceView(g_slot[i], &sv, h);
    }
    g_slotsBuilt = true;
    g_slotValid[0] = g_slotValid[1] = false;
    Log("[3D] Eye slots built (LEFT=%p, RIGHT=%p)", (void*)g_slot[0], (void*)g_slot[1]);
}

// ---- Main composite execute (called from Present hook once per frame) ----
void Compositor_Execute(IDXGISwapChain3* sc, int eye) {
    if (!g_ready) {
        if (!BuildPipeline()) return;
    }
    EnsureSlots();
    if (!g_slotsBuilt) return;

    ID3D12Device* dev = Dx12_GetDevice();
    ID3D12CommandQueue* queue = Dx12_GetDirectQueue();
    if (!dev || !queue) return;

    UINT idx = sc->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = nullptr;
    if (FAILED(sc->GetBuffer(idx, IID_ID3D12Resource, (void**)&bb)) || !bb) return;

    // eye is passed from Present hook (pop from camera queue via Afr_PopFrameEye)
    // 0 = LEFT, 1 = RIGHT

    // Ring fence wait
    int r = g_ringIdx;
    if (g_ringFence[r] && g_fence->GetCompletedValue() < g_ringFence[r]) {
        g_fence->SetEventOnCompletion(g_ringFence[r], g_fenceEvt);
        WaitForSingleObject(g_fenceEvt, 1000);
    }

    g_presentFrame++;
    g_alloc[r]->Reset();
    g_list->Reset(g_alloc[r], g_pso);

    // Check if camera hook ran this frame
    // Read the current mono mode from Present hook
    extern std::atomic<int> g_forceMonoMode;
    extern std::atomic<int> g_dualUpdate;
    int forceMonoCam = g_forceMonoMode.load(std::memory_order_acquire);
    bool camHookRan = (forceMonoCam == 0);

    // When outputMode=0 (mono/off), force mono path regardless of camera hook
    // state. This ensures both slots are updated every frame (no stale content)
    // and the shader uses mode=0 (passthrough). The compositor still runs to
    // keep D3D12 pipeline objects initialized and GPU work submitted.
    if (g_cfg.outputMode == 0) {
        forceMonoCam = 1;
        camHookRan = false;
    }
    // During grace period (stereo mode but camera hook didn't actually run),
    // update BOTH slots to prevent stale content causing black flash.
    bool dualUpdate = (g_dualUpdate.load(std::memory_order_acquire) != 0);

    // Track frame number when each slot was last updated
    static uint64_t s_slotFrame[2] = {0, 0};

    // Detect mono→stereo transition to avoid 1-frame stale slot
    // (the other eye would show old content from mono mode → black flash)
    static bool s_wasMono = false;
    bool transitionedToStereo = s_wasMono && !forceMonoCam;
    s_wasMono = forceMonoCam;

    // === STEREO MODE (quality > 70%) ===
    if (camHookRan) {
        // Update the current eye slot with fresh offset content
        Barrier(g_list, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(g_list, g_slot[eye], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        g_list->CopyResource(g_slot[eye], bb);
        Barrier(g_list, g_slot[eye], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        s_slotFrame[eye] = g_presentFrame;
        g_slotValid[eye] = true;

        // On mono→stereo transition, refresh BOTH slots to prevent
        // the other eye from showing stale (possibly black) content
        // for one frame before AFR alternation catches up.
        if (transitionedToStereo || dualUpdate) {
            int otherEye = 1 - eye;
            Barrier(g_list, g_slot[otherEye], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            g_list->CopyResource(g_slot[otherEye], bb);
            Barrier(g_list, g_slot[otherEye], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            s_slotFrame[otherEye] = g_presentFrame;
            g_slotValid[otherEye] = true;
        }

        // If the OTHER slot hasn't been updated for 3+ frames, it's stale.
        // Copy current bb to it too (mono refresh for both eyes on this frame).
        int otherEye = 1 - eye;
        if (s_slotFrame[otherEye] > 0 &&
            g_presentFrame - s_slotFrame[otherEye] > 3) {
            Barrier(g_list, g_slot[otherEye], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            g_list->CopyResource(g_slot[otherEye], bb);
            Barrier(g_list, g_slot[otherEye], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            s_slotFrame[otherEye] = g_presentFrame;
            g_slotValid[otherEye] = true;
        }
        Barrier(g_list, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    } else {
        // === MONO MODE (quality <= 70%) ===
        // Camera hook not running reliably. Force mono SBS: copy current bb
        // to BOTH slots every frame. This ensures:
        //   1. No ghosting (both slots always fresh)
        //   2. No jumping (same content in both eyes)
        //   3. Smooth transition (automatically recovers when quality improves)
        // Force mono: both slots get current bb content (no stereo offset)
        Barrier(g_list, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (int i = 0; i < 2; i++) {
            Barrier(g_list, g_slot[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            g_list->CopyResource(g_slot[i], bb);
            Barrier(g_list, g_slot[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            s_slotFrame[i] = g_presentFrame;
            g_slotValid[i] = true;
        }
        Barrier(g_list, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    // Step 2: Compute convergence (HIT offset in UV space)
    // IMPORTANT: Limit HIT offset to prevent edge clipping/mirroring
    // Maximum HIT offset should be < 5% of screen width to avoid visible edges
    const float MAX_HIT_OFFSET = 0.04f;  // 4% of screen width max
    
    float sepPerEye = StereoMath::SepPerEye(g_cfg.separationMM, g_cfg.worldUnitsPerMetre);
    float conv;
    if (g_cfg.convergence > 0.f) {
        conv = g_cfg.convergence * g_cfg.convergenceScale;
    } else if (g_cfg.convergenceDistance > 0.f) {
        conv = (sepPerEye / g_cfg.convergenceDistance) * g_cfg.convergenceScale;
    } else {
        conv = 0.f;
    }
    
    // Clamp HIT offset to maximum to prevent edge distortion
    if (conv > MAX_HIT_OFFSET) conv = MAX_HIT_OFFSET;
    if (conv < -MAX_HIT_OFFSET) conv = -MAX_HIT_OFFSET;

    // Get back buffer dimensions
    DXGI_SWAP_CHAIN_DESC1 d{};
    UINT w = 1920, h = 1080;
    if (SUCCEEDED(sc->GetDesc1(&d))) { w = d.Width; h = d.Height; }

    // Step 3: Set shader params and draw
    // Force mono when:
    //   - only one slot is valid (first frames), OR
    //   - quality metric indicates unreliable camera hook (mono mode)
    bool slotOk = (g_slotValid[0] && g_slotValid[1]);
    bool qualityMono = !camHookRan;  // mono if forceMonoMode set by Present hook
    bool monoActive = (!slotOk || qualityMono);
    float forceMono = monoActive ? 1.f : 0.f;
    float freshLeft = (eye == 0) ? 1.f : 0.f;

    // Zero convergence HIT in mono mode. In mono SBS both slots have identical
    // content; re-using non-zero conv would shift the left/right eyes by
    // ±gConv, causing the same UI text to appear at TWO different pixel
    // positions across the two halves — the brain perceives this as "ghost
    // text / double-drawn overlap". Fix by disabling HIT translation whenever
    // the stereo separation itself is disabled.
    float effectiveConv = conv;
    if (monoActive) effectiveConv = 0.0f;

    float p[12] = {
        effectiveConv, (float)w, (float)h, (float)g_cfg.outputMode,
        g_cfg.swapEyes ? 1.f : 0.f,
        0.f, 0.f,  // shiftL, shiftR
        forceMono,
        freshLeft,
        0.f, 0.f, 0.f
    };

    // Upload cbuffer data via persistent mapping
    // IMPORTANT: We write to the mapped pointer AFTER waiting for the previous frame's fence
    // to ensure the GPU is no longer reading the old data.
    if (g_paramPtr) {
        memcpy(g_paramPtr, p, sizeof(p));
        // Memory barrier to ensure writes are visible before GPU reads
        MemoryBarrier();
    }

    // Create RTV for back buffer
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(bb, nullptr, rtv);

    // Clear back buffer to black (disable for now - test passthrough)
    // float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    // g_list->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

    // Set pipeline state
    g_list->SetGraphicsRootSignature(g_root);
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap };
    g_list->SetDescriptorHeaps(1, heaps);
    g_list->SetGraphicsRootConstantBufferView(0, g_paramCB->GetGPUVirtualAddress());
    D3D12_GPU_DESCRIPTOR_HANDLE tbl = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    g_list->SetGraphicsRootDescriptorTable(1, tbl);

    // Viewport and scissor rect
    D3D12_VIEWPORT vp{0, 0, (float)w, (float)h, 0, 1};
    RECT sci{0, 0, (LONG)w, (LONG)h};
    g_list->RSSetViewports(1, &vp);
    g_list->RSSetScissorRects(1, &sci);
    g_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    // Draw full-screen triangle
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_list->SetPipelineState(g_pso);
    g_list->DrawInstanced(3, 1, 0, 0);

    // Barrier back to PRESENT
    Barrier(g_list, bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    g_list->Close();

    // Submit via REAL ECL (bypass hook)
    ID3D12CommandList* lists[] = { g_list };
    Dx12_RealExecuteCommandLists(queue, 1, lists);
    queue->Signal(g_fence, ++g_fenceVal);
    g_ringFence[r] = g_fenceVal;

    // Pipeline depth pin (N=1)
    UINT64 depth = 1;
    if (g_fenceVal > depth) {
        UINT64 waitFor = g_fenceVal - depth;
        if (g_fence->GetCompletedValue() < waitFor) {
            g_fence->SetEventOnCompletion(waitFor, g_fenceEvt);
            WaitForSingleObject(g_fenceEvt, 100);
        }
    }

    g_ringIdx = (g_ringIdx + 1) % kRing;
    bb->Release();
}

bool Compositor_Init() {
    return BuildPipeline();
}

bool Compositor_IsReady() { return g_ready; }

void Compositor_Cleanup() {
    if (g_fenceEvt) { CloseHandle(g_fenceEvt); g_fenceEvt = nullptr; }
    for (int i = 0; i < 2; i++) {
        if (g_slot[i]) { g_slot[i]->Release(); g_slot[i] = nullptr; }
    }
    for (int i = 0; i < kRing; i++) {
        if (g_alloc[i]) { g_alloc[i]->Release(); g_alloc[i] = nullptr; }
    }
    if (g_list) { g_list->Release(); g_list = nullptr; }
    if (g_pso) { g_pso->Release(); g_pso = nullptr; }
    if (g_root) { g_root->Release(); g_root = nullptr; }
    if (g_srvHeap) { g_srvHeap->Release(); g_srvHeap = nullptr; }
    if (g_rtvHeap) { g_rtvHeap->Release(); g_rtvHeap = nullptr; }
    if (g_paramCB) { g_paramCB->Release(); g_paramCB = nullptr; }
    if (g_fence) { g_fence->Release(); g_fence = nullptr; }
    g_ready = false;
    g_slotsBuilt = false;
}
