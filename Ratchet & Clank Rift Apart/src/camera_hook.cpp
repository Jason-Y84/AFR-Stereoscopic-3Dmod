// ============================================================================
//  Rift Apart 3D+6DOF - Camera hook implementation
//  AOB scan at CAMERA_ADDRESS_INTERCEPT, code cave, ApplyHeadRCRA
//  Four independent layers: tracking, stereo eye offset, Z-dolly, FOV
// ============================================================================
#include "camera_hook.h"
#include "config.h"
#include "logger.h"
#include "opentrack_receiver.h"
#include "afr_engine.h"       // Afr_FrameEye, Afr_ReportCameraEye
#include "stereo_math.h"
#include "dx12_hooks.h"        // Stereo_NoteCameraWrite, Dx12_GetFrameEye
#include <psapi.h>
#include <cmath>
#include <cstring>

using namespace P5HT;

#define PI 3.14159265358979323846f

// Camera struct pointer (defined here, declared extern in header)
extern "C" volatile uint64_t g_cam = 0;  // NOLINT: extern with init is the definition

// ---- shared state (was in dllmain.cpp) ----
static bool g_haveZero = false;
static Pose g_zero;
static long long s_last = 0;
static double s_wob = 0;
static float s_hy=0,s_hp=0,s_hr=0,s_lx=0,s_ly=0,s_lz=0;
static float s_vy=0,s_vp=0,s_vr=0,s_vx=0,s_vyy=0,s_vz=0;

// ---- Accumulation guard: detect if game reused our modified matrix ----
// Tracks what we last wrote (g_lastWrittenXxx) and the original base (g_cleanXxx).
// If current matrix matches last written → game reused it → use clean base.
// If different → game updated → use current as new base. Transforms always applied.
static bool g_haveLastWritten = false;
static float g_lastWrittenR[3] = {0,0,0};
static float g_lastWrittenU[3] = {0,0,0};
static float g_lastWrittenF[3] = {0,0,0};
static float g_lastWrittenP[3] = {0,0,0};
static float g_cleanR[3] = {0,0,0};  // original base when we last wrote
static float g_cleanU[3] = {0,0,0};
static float g_cleanF[3] = {0,0,0};
static float g_cleanP[3] = {0,0,0};
// Threshold for "game reused our matrix" detection.
// 1e-4: matrix components match within ~0.01% (float precision for reuse).
// Game updates (camera movement, animation) produce diffs >> 1e-4.
// Reuse (game reads & writes back unchanged) produces diff ≈ 0.
static const float REUSE_THRESHOLD = 1e-4f;
static volatile float g_hy=0,g_hp=0,g_hr=0,g_lean[3]={0,0,0};
extern OpenTrackReceiver g_recv;

std::atomic<uint64_t> g_camHookCounter{0};  // incremented by camera hook each call
std::atomic<int> g_forceMonoMode{0};  // set by Present hook when camera hook misses
std::atomic<int> g_dualUpdate{0};     // set by Present hook: 1 when camera hook didn't run this frame (grace period)

static inline float clampf(float v,float lo,float hi){return v<lo?lo:(v>hi?hi:v);}
static inline float lim(float v,float L){ if(L<=0.f) return v; if(!g_cfg.softLimit) return clampf(v,-L,L); return L*tanhf(v/L); }

struct HD{float hy,hp,hr,lx,ly,lz;};

static HD Wobble(float dt){
    s_wob+=dt; HD d{0,0,0,0,0,0}; const float seg=2.5f; int idx=((int)(s_wob/seg))%6;
    float ph=(float)fmod(s_wob,seg)/seg; float s=sinf(ph*2*PI);
    switch(idx){case 0:d.hy=s*g_cfg.wobbleDeg;break;case 1:d.hp=s*g_cfg.wobbleDeg;break;
        case 2:d.hr=s*g_cfg.wobbleDeg;break;case 3:d.lx=s*g_cfg.wobbleLean;break;
        case 4:d.ly=s*g_cfg.wobbleLean;break;case 5:d.lz=s*g_cfg.wobbleLean;break;}
    d.hy*=(g_cfg.invYaw?-1:1);d.hp*=(g_cfg.invPitch?-1:1);d.hr*=(g_cfg.invRoll?-1:1);
    d.lx*=(g_cfg.invX?-1:1);d.ly*=(g_cfg.invY?-1:1);d.lz*=(g_cfg.invZ?-1:1);
    return d;
}

static HD Track(const Pose& r){
    if(!g_haveZero){g_zero=r;g_haveZero=true;}
    float dy=r.yaw-g_zero.yaw, dp=r.pitch-g_zero.pitch, dr=r.roll-g_zero.roll;
    float dx=r.x-g_zero.x, dyv=r.y-g_zero.y, dz=r.z-g_zero.z;
    HD d;
    d.hy=lim(dy*g_cfg.yawSens*g_cfg.globalScale,g_cfg.limitDeg)*(g_cfg.invYaw?-1:1);
    d.hp=lim(dp*g_cfg.pitchSens*g_cfg.globalScale,g_cfg.limitDeg)*(g_cfg.invPitch?-1:1);
    d.hr=lim(dr*g_cfg.rollSens*g_cfg.globalScale,g_cfg.limitDeg)*(g_cfg.invRoll?-1:1);
    if(g_cfg.swapYZ){float t=dyv;dyv=dz;dz=t;}
    d.lx=lim(dx*g_cfg.leanScaleX*g_cfg.leanScale,g_cfg.leanLimit)*(g_cfg.invX?-1:1);
    d.ly=lim(dyv*g_cfg.leanScaleY*g_cfg.leanScale,g_cfg.leanLimit)*(g_cfg.invY?-1:1);
    d.lz=lim(dz*g_cfg.leanScaleZ*g_cfg.leanScale,g_cfg.leanLimit)*(g_cfg.invZ?-1:1);
    return d;
}

static HD Smooth(HD t,float dt){
    if(g_cfg.smoothMode==1){
        float w=2*PI*clampf(g_cfg.smoothHz,0.1f,30.f), et=expf(-w*dt);
        auto st=[&](float&x,float&v,float tg){float c1=x-tg,c2=v+w*c1;x=tg+(c1+c2*dt)*et;v=(c2-w*(c1+c2*dt))*et;};
        st(s_hy,s_vy,t.hy);st(s_hp,s_vp,t.hp);st(s_hr,s_vr,t.hr);
        st(s_lx,s_vx,t.lx);st(s_ly,s_vyy,t.ly);st(s_lz,s_vz,t.lz);
    } else {
        float a=clampf(g_cfg.smoothEMA,0.01f,1.f);
        s_hy+=(t.hy-s_hy)*a;s_hp+=(t.hp-s_hp)*a;s_hr+=(t.hr-s_hr)*a;
        s_lx+=(t.lx-s_lx)*a;s_ly+=(t.ly-s_ly)*a;s_lz+=(t.lz-s_lz)*a;
    }
    return HD{s_hy,s_hp,s_hr,s_lx,s_ly,s_lz};
}

void CameraUpdate(){
    long long now=(long long)GetTickCount64();
    float dt=s_last?(now-s_last)/1000.f:0.016f;
    if(dt<=0) dt=0.016f; if(dt>0.1f) dt=0.1f; s_last=now;

    if(g_tog.exchange(false)){g_en=!g_en; Log("head-tracking %s",g_en?"ON":"OFF");}
    if(g_rec.exchange(false)){g_haveZero=false; Log("recenter");}

    // When tracking off, zero the smoothed head values
    if(!g_en){
        g_hy=g_hp=g_hr=0; g_lean[0]=g_lean[1]=g_lean[2]=0;
        // Don't return! Stereo/Z/FOV must still run.
    } else {
        HD tg=g_cfg.wobbleTest?Wobble(dt):(g_recv.IsReceiving()?Track(g_recv.Latest()):HD{0,0,0,0,0,0});
        HD h=Smooth(tg,dt);
        g_hy=h.hy; g_hp=h.hp; g_hr=h.hr;
        g_lean[0]=h.lx; g_lean[1]=h.ly; g_lean[2]=h.lz;
    }

    static int bt=0;
    if(g_cfg.diagnostics&&(++bt%300)==0)
        Log("tick head(y%.1f p%.1f r%.1f) lean(%.2f %.2f %.2f) recv=%d cam=%llX",
            g_hy,g_hp,g_hr,g_lean[0],g_lean[1],g_lean[2],
            (int)g_recv.IsReceiving(),(unsigned long long)g_cam);
}

// ---- ApplyHeadRCRA: called from the code cave after stolen bytes ----
// Layers: head tracking rotation/lean, stereo eye offset + shear, Z-dolly, FOV
extern "C" __attribute__((used,noinline)) void ApplyHeadRCRA(uint64_t cam){
    if(!cam) return;
    uint8_t* base=(uint8_t*)cam;
    float* m=(float*)(base + g_cfg.matrixOffset);   // 4x4 @ +0x0C
    if(IsBadWritePtr(m,64)) return;

    // Signal Present hook that we're alive. Increment before orthonormality
    // check so cutscene transitions with degenerate matrices don't trigger mono.
    if(g_cfg.stereoEnabled){
        g_camHookCounter.fetch_add(1, std::memory_order_relaxed);
    }

    g_cam = cam;  // expose latest cam addr for `tick head` log

    // Orthonormality check — reject non-view matrices (e.g. transposed or
    // projection fragments) before touching anything.
    float* R=&m[0]; float* U=&m[4]; float* F=&m[8]; float* P=&m[12];
    auto Ln=[](const float*v){return v[0]*v[0]+v[1]*v[1]+v[2]*v[2];};
    float lr=Ln(R),lu=Ln(U),lf=Ln(F);
    if(lr<0.5f||lr>2.f||lu<0.5f||lu>2.f||lf<0.5f||lf>2.f){
        return;
    }

    // Camera type detection: game cameras have P[1] in 20-70, P[2] < -50.
    // Menu/ortho/top-down cameras skip shear (use eye offset only).
    bool isGameCamera = (P[1] > 20.0f && P[1] < 70.0f && P[2] < -50.0f);
    bool isOrthoCamera = (fabsf(fabsf(R[0])-1)<1e-4f && fabsf(R[1])<1e-4f && fabsf(R[2])<1e-4f &&
                          fabsf(U[0])<1e-4f && fabsf(fabsf(U[1])-1)<1e-4f && fabsf(U[2])<1e-4f &&
                          fabsf(F[0])<1e-4f && fabsf(F[1])<1e-4f && fabsf(fabsf(F[2])-1)<1e-4f);
    // Skip shear for non-game cameras and ortho cameras
    bool skipShear = !isGameCamera || isOrthoCamera;

    // Accumulation guard: choose base matrix.
    float curR[3]={R[0],R[1],R[2]};
    float curU[3]={U[0],U[1],U[2]};
    float curF[3]={F[0],F[1],F[2]};
    float curP[3]={P[0],P[1],P[2]};

    float baseR[3], baseU[3], baseF[3], baseP[3];
    bool gameReusedMatrix = false;

    if(g_haveLastWritten){
        // Compare current matrix with what we last wrote
        float dr = (curR[0]-g_lastWrittenR[0])*(curR[0]-g_lastWrittenR[0])
                 + (curR[1]-g_lastWrittenR[1])*(curR[1]-g_lastWrittenR[1])
                 + (curR[2]-g_lastWrittenR[2])*(curR[2]-g_lastWrittenR[2]);
        float du = (curU[0]-g_lastWrittenU[0])*(curU[0]-g_lastWrittenU[0])
                 + (curU[1]-g_lastWrittenU[1])*(curU[1]-g_lastWrittenU[1])
                 + (curU[2]-g_lastWrittenU[2])*(curU[2]-g_lastWrittenU[2]);
        float df = (curF[0]-g_lastWrittenF[0])*(curF[0]-g_lastWrittenF[0])
                 + (curF[1]-g_lastWrittenF[1])*(curF[1]-g_lastWrittenF[1])
                 + (curF[2]-g_lastWrittenF[2])*(curF[2]-g_lastWrittenF[2]);
        float dp = (curP[0]-g_lastWrittenP[0])*(curP[0]-g_lastWrittenP[0])
                 + (curP[1]-g_lastWrittenP[1])*(curP[1]-g_lastWrittenP[1])
                 + (curP[2]-g_lastWrittenP[2])*(curP[2]-g_lastWrittenP[2]);
        float totalDiff = dr + du + df + dp;

        if(totalDiff < REUSE_THRESHOLD){
            // Game reused our modified matrix → use clean base to prevent
            // accumulation. This is the menu/controller intermittent case.
            gameReusedMatrix = true;
            baseR[0]=g_cleanR[0]; baseR[1]=g_cleanR[1]; baseR[2]=g_cleanR[2];
            baseU[0]=g_cleanU[0]; baseU[1]=g_cleanU[1]; baseU[2]=g_cleanU[2];
            baseF[0]=g_cleanF[0]; baseF[1]=g_cleanF[1]; baseF[2]=g_cleanF[2];
            baseP[0]=g_cleanP[0]; baseP[1]=g_cleanP[1]; baseP[2]=g_cleanP[2];
        } else {
            // Game updated the matrix (camera moved, animation, new frame)
            // → use current as fresh base, update clean tracking
            gameReusedMatrix = false;
            baseR[0]=curR[0]; baseR[1]=curR[1]; baseR[2]=curR[2];
            baseU[0]=curU[0]; baseU[1]=curU[1]; baseU[2]=curU[2];
            baseF[0]=curF[0]; baseF[1]=curF[1]; baseF[2]=curF[2];
            baseP[0]=curP[0]; baseP[1]=curP[1]; baseP[2]=curP[2];
            g_cleanR[0]=baseR[0]; g_cleanR[1]=baseR[1]; g_cleanR[2]=baseR[2];
            g_cleanU[0]=baseU[0]; g_cleanU[1]=baseU[1]; g_cleanU[2]=baseU[2];
            g_cleanF[0]=baseF[0]; g_cleanF[1]=baseF[1]; g_cleanF[2]=baseF[2];
            g_cleanP[0]=baseP[0]; g_cleanP[1]=baseP[1]; g_cleanP[2]=baseP[2];
        }
    } else {
        // First call ever — use current as base
        gameReusedMatrix = false;
        baseR[0]=curR[0]; baseR[1]=curR[1]; baseR[2]=curR[2];
        baseU[0]=curU[0]; baseU[1]=curU[1]; baseU[2]=curU[2];
        baseF[0]=curF[0]; baseF[1]=curF[1]; baseF[2]=curF[2];
        baseP[0]=curP[0]; baseP[1]=curP[1]; baseP[2]=curP[2];
        g_cleanR[0]=baseR[0]; g_cleanR[1]=baseR[1]; g_cleanR[2]=baseR[2];
        g_cleanU[0]=baseU[0]; g_cleanU[1]=baseU[1]; g_cleanU[2]=baseU[2];
        g_cleanF[0]=baseF[0]; g_cleanF[1]=baseF[1]; g_cleanF[2]=baseF[2];
        g_cleanP[0]=baseP[0]; g_cleanP[1]=baseP[1]; g_cleanP[2]=baseP[2];
        g_haveLastWritten = true;
    }

    // Copies for modification
    float Rc[3]={baseR[0],baseR[1],baseR[2]};
    float Uc[3]={baseU[0],baseU[1],baseU[2]};
    float Fc[3]={baseF[0],baseF[1],baseF[2]};
    float Pc[3]={baseP[0],baseP[1],baseP[2]};

    // Original forward/up/right for Toe-in rotation (Rodrigues uses F0 as
    // the base vector — prevents toe-in accumulation across calls).
    float R0[3]={Rc[0],Rc[1],Rc[2]};
    float U0[3]={Uc[0],Uc[1],Uc[2]};
    float F0[3]={Fc[0],Fc[1],Fc[2]};

    // ---- LAYER 1: Head tracking rotation (on copies) ----
    if(g_en && g_cfg.camRotEnabled){
        const float D2R=0.01745329252f;
        float yaw=g_hy*g_cfg.camRotScale*D2R, pit=g_hp*g_cfg.camRotScale*D2R, rol=g_hr*g_cfg.camRotScale*D2R;
        if(g_cfg.matrixTranspose){ yaw=-yaw; pit=-pit; }
        auto rot2=[](float*a,float*b,float ang){ float c=cosf(ang),s=sinf(ang);
            for(int i=0;i<3;i++){ float x=a[i],y=b[i]; a[i]=c*x+s*y; b[i]=-s*x+c*y; } };
        rot2(Rc,Fc,yaw);   // yaw about up
        rot2(Fc,Uc,pit);   // pitch about right
        rot2(Rc,Uc,rol);   // roll about forward
    }

    // ---- LAYER 1: Head tracking lean (on copies) ----
    if(g_en && g_cfg.camPosEnabled){
        Pc[0]+= g_lean[0]*R0[0]+g_lean[1]*U0[0]+g_lean[2]*F0[0];
        Pc[1]+= g_lean[0]*R0[1]+g_lean[1]*U0[1]+g_lean[2]*F0[1];
        Pc[2]+= g_lean[0]*R0[2]+g_lean[1]*U0[2]+g_lean[2]*F0[2];
    }

    // ---- LAYER 2: Stereo eye offset + Shear convergence (off-axis) ----
    // Shear (non-orthonormal basis) instead of toe-in rotation eliminates
    // trapezoidal distortion. Only F[0] is modified; R, U stay unchanged.
    // Matrix is CAMERA-TO-WORLD: shader inverts it, flipping the shear sign.
    // Sign = -eyeSign for convergence (camera-to-world convention).
    // Gated on outputMode != 0: mono mode skips this to avoid jitter.
    if(g_cfg.stereoEnabled && g_cfg.outputMode != 0){
        int eye = Afr_FrameEye();
        float sep = StereoMath::SepPerEye(g_cfg.separationMM, g_cfg.worldUnitsPerMetre);
        if(g_cfg.separationMM == 0.f) sep = g_cfg.separationX;
        float eyeSign = (eye == 0) ? -1.0f : +1.0f;  // slot 0 = LEFT

        // ---- Eye offset FIRST (uses Rc after head tracking) ----
        // eye offset shifts camera position in view space
        Pc[0] += eyeSign * sep * Rc[0];
        Pc[1] += eyeSign * sep * Rc[1];
        Pc[2] += eyeSign * sep * Rc[2];

        // ---- Shear convergence: tilt F[0] toward convergence point ----
        // Sign = -eyeSign (camera-to-world: shader inversion flips sign).
        // Directly modifying F[0] (not multiplying by Fc[2]) makes the fix
        // independent of F[2] sign convention. Skipped for non-game cameras
        // (menu/ortho/top-down) to avoid DLSS/tilt artifacts.
        if(g_cfg.convergenceDistance > 0.001f && sep > 0.f){
            // Detect top-down camera: forward vector points mostly along Y axis
            bool isTopDownCamera = (fabsf(Fc[1]) > 0.7f);

            if(!skipShear && !isTopDownCamera){
                float shearFactor = 1.0f - g_cfg.convergenceScale;
                if (shearFactor < 0.0f) shearFactor = 0.0f;
                if (shearFactor > 1.0f) shearFactor = 1.0f;
                // Sign = -eyeSign for camera-to-world convergence (see above)
                float shearAmount = -eyeSign * sep / g_cfg.convergenceDistance * shearFactor;

                // Directly tilt F[0] toward convergence point.
                // Only F[0] is modified: R and U stay unchanged, so the basis
                // becomes non-orthonormal (a shear, not a rotation). This
                // avoids the keystone/vertical disparity that pure toe-in
                // rotation would cause on UI elements.
                Fc[0] += shearAmount;
            }
        }
    }

    // ---- LAYER 4: Z-dolly (on copies) ----
    if(g_cfg.zEnabled){
        float z = g_cfg.zOffset * g_cfg.worldUnitsPerMetre;
        Pc[0] += z * Fc[0]; Pc[1] += z * Fc[1]; Pc[2] += z * Fc[2];
    }

    // ---- WRITE modified copies back to matrix ----
    R[0]=Rc[0]; R[1]=Rc[1]; R[2]=Rc[2];
    U[0]=Uc[0]; U[1]=Uc[1]; U[2]=Uc[2];
    F[0]=Fc[0]; F[1]=Fc[1]; F[2]=Fc[2];
    P[0]=Pc[0]; P[1]=Pc[1]; P[2]=Pc[2];

    // Update accumulation guard tracking: record what we just wrote so the
    // next call can detect whether the game preserved our modification.
    g_lastWrittenR[0]=R[0]; g_lastWrittenR[1]=R[1]; g_lastWrittenR[2]=R[2];
    g_lastWrittenU[0]=U[0]; g_lastWrittenU[1]=U[1]; g_lastWrittenU[2]=U[2];
    g_lastWrittenF[0]=F[0]; g_lastWrittenF[1]=F[1]; g_lastWrittenF[2]=F[2];
    g_lastWrittenP[0]=P[0]; g_lastWrittenP[1]=P[1]; g_lastWrittenP[2]=P[2];

    Stereo_NoteCameraWrite();
}

// ---- AOB scan helper ----
static uint8_t* FindAOB(uint8_t* mod, size_t sz, const char* pat){
    uint8_t by[128]; int mk[128], n=0; const char* p=pat;
    while(*p && n<128){ while(*p==' ')p++; if(!*p)break;
        if(p[0]=='?'){mk[n]=0;by[n]=0;}else{mk[n]=1;by[n]=(uint8_t)strtol(p,0,16);}
        n++; p+=2; }
    for(size_t i=0;i+n<sz;i++){ int k=0; for(;k<n;k++) if(mk[k]&&mod[i+k]!=by[k])break; if(k==n) return mod+i; }
    return 0;
}

static uint8_t* g_cave = 0;
static uint8_t* g_aobAddr = 0;       // saved AOB address for re-check
static uint8_t* g_modBase = 0;       // saved module base for re-search
static size_t g_modSize = 0;         // saved module size for re-search

// Check if the camera hook is still installed (jump bytes still present)
bool CameraHook_IsInstalled(){
    if(!g_aobAddr) return false;
    return (g_aobAddr[0] == 0xFF && g_aobAddr[1] == 0x25);
}

// Try to reinstall the hook if it was removed by the game
// force=true: would re-scan for additional AOB locations
// BUT: if the hook is still installed (jump bytes present), the game just isn't
// using this code path yet (e.g., in menu/loading screen). Re-scanning won't help
// because FindAOB can't find the pattern (it's been patched). Just wait.
bool CameraHook_Reinstall(bool force){
    if(!g_modBase || !g_modSize) return false;

    // If hook is still installed, don't re-scan - just wait for game to use it
    if(CameraHook_IsInstalled()) {
        return true;  // hook is fine, game will use it when ready
    }

    // Hook was removed (jump bytes gone) - search for AOB again
    Log("[3D][CAM] Hook removed by game - searching for AOB again...");
    g_cave = nullptr;
    g_aobAddr = nullptr;
    return CameraHook_Install(g_modBase, g_modSize);
}

bool CameraHook_Install(uint8_t* mod, size_t sz){
    // Save module info for later re-search
    g_modBase = mod;
    g_modSize = sz;

    // CAMERA_ADDRESS_INTERCEPT: movups [rbx+0xC],xmm0 ; ... ; movups [rbx+0x3C],xmm1
    uint8_t* a=FindAOB(mod,sz,"0F 11 43 0C 89 44 24 ?? 0F 28 84 24 ?? ?? ?? ?? 0F 11 4B 1C 0F 28 8C 24 ?? ?? ?? ?? 0F 11 43 2C 0F 11 4B 3C");
    if(!a){
        Log("hook: CAMERA_ADDRESS_INTERCEPT not found in module");
        return false;
    }

    const int STOLEN=36;
    // Reuse existing cave if already allocated (for reinstall)
    if(!g_cave) g_cave=(uint8_t*)VirtualAlloc(0,512,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    if(!g_cave){ Log("hook: cave alloc failed"); return false; }
    uint8_t* c=g_cave;  // Reset cave pointer to start
    auto B=[&](uint8_t v){*c++=v;};
    auto D64=[&](uint64_t v){memcpy(c,&v,8);c+=8;};
    auto D32=[&](uint32_t v){memcpy(c,&v,4);c+=4;};

    // Re-emit stolen bytes (writes matrix to [rbx+0xC])
    memcpy(c,a,STOLEN); c+=STOLEN;

    // Store camera struct pointer for other modules
    B(0x48);B(0xB8);D64((uint64_t)&g_cam); B(0x48);B(0x89);B(0x18);   // mov rax,&g_cam ; mov [rax],rbx

    // Save volatile GP regs
    B(0x50);B(0x51);B(0x52); B(0x41);B(0x50); B(0x41);B(0x51); B(0x41);B(0x52); B(0x41);B(0x53);
    B(0x55);                                   // push rbp
    B(0x48);B(0x89);B(0xE5);                   // mov rbp,rsp
    B(0x48);B(0x83);B(0xE4);B(0xF0);           // and rsp,-16
    B(0x48);B(0x81);B(0xEC);D32(0x80);         // sub rsp,0x80
    for(int i=0;i<6;i++){ B(0x0F);B(0x11);B((uint8_t)(0x84|(i<<3)));B(0x24);D32(0x20+i*16); }
    B(0x48);B(0x89);B(0xD9);                   // mov rcx,rbx
    B(0x48);B(0xB8);D64((uint64_t)&ApplyHeadRCRA); B(0xFF);B(0xD0);
    for(int i=0;i<6;i++){ B(0x0F);B(0x10);B((uint8_t)(0x84|(i<<3)));B(0x24);D32(0x20+i*16); }
    B(0x48);B(0x89);B(0xEC);
    B(0x5D);
    B(0x41);B(0x5B); B(0x41);B(0x5A); B(0x41);B(0x59); B(0x41);B(0x58); B(0x5A);B(0x59);B(0x58);
    B(0xFF);B(0x25);D32(0); D64((uint64_t)(a+STOLEN));   // jmp back

    // Patch the game: 14-byte absolute jump
    DWORD old;
    VirtualProtect(a,STOLEN,PAGE_EXECUTE_READWRITE,&old);
    a[0]=0xFF; a[1]=0x25; *(uint32_t*)(a+2)=0; *(uint64_t*)(a+6)=(uint64_t)g_cave;
    for(int i=14;i<STOLEN;i++) a[i]=0x90;
    VirtualProtect(a,STOLEN,old,&old);
    FlushInstructionCache(GetCurrentProcess(),a,STOLEN);

    // Save AOB address for later re-check
    g_aobAddr = a;

    Log("hook: CAMERA_ADDRESS_INTERCEPT @ module+%llX (rbx=cam; matrix@+0x%X FOV@+0x70)",
        (unsigned long long)(a-mod), g_cfg.matrixOffset);
    return true;
}

// ---- FOV hook (separate AOB) ----
static uint8_t* g_fovCave = 0;
static int g_fovDisp = 0x70;
extern "C" { extern volatile uint64_t g_fovPtr; extern volatile int g_fovOn; extern volatile float g_fovVal; }

bool FovHook_Install(uint8_t* mod, size_t sz){
    uint8_t* a=FindAOB(mod,sz,"F3 0F 5D 40 ?? F3 0F 11 41 ?? 84 D2 74 07 C6 81 ?? ?? ?? ?? 01");
    if(!a){ static int w=0; if(!w++) Log("fov: waiting for CAMERA_FOV_WRITE..."); return false; }
    g_fovDisp=a[9];
    uint8_t* h=a+5; const int STOLEN=16;
    g_fovCave=(uint8_t*)VirtualAlloc(0,256,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    if(!g_fovCave) return false;

    uint8_t* c=g_fovCave;
    auto B=[&](uint8_t v){*c++=v;};
    auto D32=[&](uint32_t v){memcpy(c,&v,4);c+=4;};
    auto D64=[&](uint64_t v){memcpy(c,&v,8);c+=8;};

    memcpy(c,h,STOLEN); c+=STOLEN;
    B(0x50); B(0x9C);                                 // push rax ; pushfq
    B(0x48);B(0x83);B(0xEC);B(0x10);                  // sub rsp,0x10
    B(0x0F);B(0x11);B(0x04);B(0x24);                  // movups [rsp],xmm0
    B(0x48);B(0xB8);D64((uint64_t)&g_fovPtr); B(0x48);B(0x89);B(0x08);
    B(0x48);B(0xB8);D64((uint64_t)&g_fovOn);  B(0x83);B(0x38);B(0x00);
    B(0x74);B(0x16);
    B(0x48);B(0xB8);D64((uint64_t)&g_fovVal);
    B(0xF3);B(0x0F);B(0x10);B(0x00);
    B(0xF3);B(0x0F);B(0x11);B(0x81);D32((uint32_t)g_fovDisp);
    B(0x0F);B(0x10);B(0x04);B(0x24);
    B(0x48);B(0x83);B(0xC4);B(0x10);
    B(0x9D); B(0x58);
    B(0xFF);B(0x25);D32(0); D64((uint64_t)(h+STOLEN));

    DWORD old;
    VirtualProtect(h,STOLEN,PAGE_EXECUTE_READWRITE,&old);
    h[0]=0xFF; h[1]=0x25; *(uint32_t*)(h+2)=0; *(uint64_t*)(h+6)=(uint64_t)g_fovCave;
    for(int i=14;i<STOLEN;i++) h[i]=0x90;
    VirtualProtect(h,STOLEN,old,&old);
    FlushInstructionCache(GetCurrentProcess(),h,STOLEN);

    Log("fov: CAMERA_FOV_WRITE @ module+%llX (fov@+0x%X read from code)",
        (unsigned long long)(a-mod), g_fovDisp);
    return true;
}
