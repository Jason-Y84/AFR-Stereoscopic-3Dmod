// ============================================================================
//  Granblue Fantasy Relink - 6DOF OpenTrack head-tracking  (64-bit)
//  Camera hook derived from the IGCS camera tool (ghostinthecamera/hattiwatti,
//  Otis_Inf IGCS). The game's camera world matrix is copied at granblue_fantasy_relink.exe+B12D58:
//     movaps xmm,[rbx+0x160/170/180/190]  ->  [rdi+0x80..0xB0]
//  rbx = camera struct, 4x4 matrix @ +0x160. We add the head pose to that matrix
//  right before the copy, so the rendered view gets 6DOF with no distortion.
// ============================================================================
#include <winsock2.h>
#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <string>
#include "opentrack_receiver.h"
#include "logger.h"
#include "stereo_dx11.h"
using namespace P5HT;

#define PI 3.14159265358979323846f

// ------------------------------------------------------------------ config ----
struct Config {
    int   port=4242, autoEnable=1, diagnostics=1, hotkeysEnabled=1;
    int   toggleKey=VK_END, recenterKey=VK_HOME;
    int   wobbleTest=0, wobbleMode=1, wobbleAxis=0;
    float wobbleDeg=12.f, wobbleLean=0.5f, wobbleHz=0.4f;
    float yawSens=0.6f,pitchSens=0.6f,rollSens=0.6f,globalScale=1.f,limitDeg=80.f;
    int   invYaw=0,invPitch=0,invRoll=0;
    int   camRotEnabled=1,camPosEnabled=1,matrixTranspose=0;
    float camRotScale=1.5f;
    float leanScale=4.f,leanScaleX=4.f,leanScaleY=4.f,leanScaleZ=5.f,leanLimit=6.f;
    int   invX=0,invY=1,invZ=1,swapYZ=0;
    int   fovEnabled=0; float fovValue=0.87f; int fovOffset=0x178;
    int   smoothMode=1; float smoothHz=10.f, smoothEMA=0.35f;
    int   matrixOffset=0x160;   // camera matrix offset within the struct (rbx+0x160)
} g;
static Config& C(){ return g; }

// --------------------------------------------------------------- ini load ----
static std::string IniPath(){ char p[MAX_PATH]; GetModuleFileNameA((HMODULE)SelfModule(),p,MAX_PATH);
    char* s=strrchr(p,'\\'); if(s)strcpy(s+1,"3D-6DOF config.ini"); return std::string(p); }
static int   GI(const char*s,const char*k,int d){ return (int)GetPrivateProfileIntA(s,k,d,IniPath().c_str()); }
static float GF(const char*s,const char*k,float d){ char b[64],e[64]; snprintf(e,64,"%.6f",d);
    GetPrivateProfileStringA(s,k,e,b,64,IniPath().c_str()); return (float)atof(b); }
static int GK(const char*s,const char*k,int d){ char b[64]; GetPrivateProfileStringA(s,k,"",b,64,IniPath().c_str());
    std::string v(b); size_t a=v.find_first_not_of(" \t"); if(a==std::string::npos)return d; v=v.substr(a);
    if(v.size()>1&&v[0]=='0'&&(v[1]=='x'||v[1]=='X'))return (int)strtol(v.c_str(),0,16);
    if(isdigit((unsigned char)v[0]))return (int)strtol(v.c_str(),0,10);
    for(auto&c:v)c=(char)toupper((unsigned char)c);
    struct{const char*n;int vk;}M[]={{"END",VK_END},{"HOME",VK_HOME},{"INSERT",VK_INSERT},{"DELETE",VK_DELETE},
      {"F1",VK_F1},{"F2",VK_F2},{"F3",VK_F3},{"F4",VK_F4},{"F5",VK_F5},{"F6",VK_F6},{"F7",VK_F7},{"F8",VK_F8}};
    for(auto&m:M) if(v==m.n)return m.vk; if(v.size()==1)return (int)v[0]; return d; }
static void LoadConfig(){
    g.port=GI("Network","UDPPort",g.port);
    g.autoEnable=GI("General","AutoEnable",g.autoEnable); g.diagnostics=GI("General","Diagnostics",g.diagnostics);
    g.hotkeysEnabled=GI("Hotkeys","Enabled",g.hotkeysEnabled);
    g.toggleKey=GK("Hotkeys","ToggleKey",g.toggleKey); g.recenterKey=GK("Hotkeys","RecenterKey",g.recenterKey);
    g.wobbleTest=GI("Test","WobbleTest",g.wobbleTest); g.wobbleMode=GI("Test","WobbleMode",g.wobbleMode);
    g.wobbleAxis=GI("Test","WobbleAxis",g.wobbleAxis); g.wobbleDeg=GF("Test","WobbleDeg",g.wobbleDeg);
    g.wobbleLean=GF("Test","WobbleLean",g.wobbleLean); g.wobbleHz=GF("Test","WobbleHz",g.wobbleHz);
    g.yawSens=GF("Rotation","YawSensitivity",g.yawSens); g.pitchSens=GF("Rotation","PitchSensitivity",g.pitchSens);
    g.rollSens=GF("Rotation","RollSensitivity",g.rollSens); g.globalScale=GF("Rotation","GlobalScale",g.globalScale);
    g.limitDeg=GF("Rotation","LimitDeg",g.limitDeg);
    g.invYaw=GI("Rotation","InvertYaw",g.invYaw); g.invPitch=GI("Rotation","InvertPitch",g.invPitch); g.invRoll=GI("Rotation","InvertRoll",g.invRoll);
    g.camRotEnabled=GI("Camera","RotationEnabled",g.camRotEnabled); g.camPosEnabled=GI("Camera","PositionEnabled",g.camPosEnabled);
    g.matrixTranspose=GI("Camera","MatrixTranspose",g.matrixTranspose); g.camRotScale=GF("Camera","RotationScale",g.camRotScale);
    g.matrixOffset=GI("Camera","MatrixOffset",g.matrixOffset);
    g.leanScale=GF("Position","LeanScale",g.leanScale); g.leanScaleX=GF("Position","LeanScaleX",g.leanScaleX);
    g.leanScaleY=GF("Position","LeanScaleY",g.leanScaleY); g.leanScaleZ=GF("Position","LeanScaleZ",g.leanScaleZ);
    g.leanLimit=GF("Position","LeanLimit",g.leanLimit);
    g.invX=GI("Position","InvertX",g.invX); g.invY=GI("Position","InvertY",g.invY); g.invZ=GI("Position","InvertZ",g.invZ);
    g.swapYZ=GI("Position","SwapYZ",g.swapYZ);
    g.fovEnabled=GI("FOV","Enabled",g.fovEnabled); g.fovValue=GF("FOV","Value",g.fovValue); g.fovOffset=GI("FOV","Offset",g.fovOffset);
    g.smoothMode=GI("Filter","SmoothMode",g.smoothMode); g.smoothHz=GF("Filter","SmoothHz",g.smoothHz); g.smoothEMA=GF("Filter","SmoothEMA",g.smoothEMA);
    Log("config: port=%d auto=%d wobble=%d matOff=0x%X rot(gs%.2f lim%.0f inv%d%d%d) pos(gs%.2f xyz=%.2f/%.2f/%.2f lim%.2f inv%d%d%d) transpose=%d",
        g.port,g.autoEnable,g.wobbleTest,g.matrixOffset,g.camRotScale,g.limitDeg,g.invYaw,g.invPitch,g.invRoll,
        g.leanScale,g.leanScaleX,g.leanScaleY,g.leanScaleZ,g.leanLimit,g.invX,g.invY,g.invZ,g.matrixTranspose);
}

// ------------------------------------------------------------------ state ----
static std::atomic<bool> g_toggleReq{false}, g_recenterReq{false};
static bool g_enabled=false, g_haveZero=false;
static Pose g_zero; static OpenTrackReceiver g_recv;
static long long s_lastMs=0; static double s_wobbleT=0;
static float s_hy=0,s_hp=0,s_hr=0,s_lx=0,s_ly=0,s_lz=0, s_vy=0,s_vp=0,s_vr=0,s_vx=0,s_vyv=0,s_vz=0;
static volatile float g_headYaw=0,g_headPitch=0,g_headRoll=0, g_lean[4]={0,0,0,0};

// applied to the camera 4x4 (rows: 0..2 basis, row3 translation). cdecl->win64: rcx=matrix ptr.
static int g_posOff=0x10, g_fwdOff=0x20, g_upOff=0x30;   // read from the AOB match -> version-agnostic
extern "C" __attribute__((used,noinline)) void ApplyHeadToGBFRCamera(uint8_t* cam){
    static volatile long s_cc=0; long cc=InterlockedIncrement(&s_cc);
    if(!cam) return;
    if(IsBadWritePtr(cam+g_posOff,0x30)){ if(cc<=3)Log("cam call#%ld BAD ptr %p",cc,cam); return; }
    Gbfr3D::Stereo_NoteCameraWrite();
    float* pos=(float*)(cam+g_posOff);
    float* tgt=(float*)(cam+g_fwdOff);
    float* U  =(float*)(cam+g_upOff);
    if(C().diagnostics && cc==1) Log("cam confirmed pos[%.1f %.1f %.1f] lookAt[%.1f %.1f %.1f]",pos[0],pos[1],pos[2],tgt[0],tgt[1],tgt[2]);
    if(C().fovEnabled){ int fo=C().fovOffset?C().fovOffset:0x178; float* fp=(float*)(cam+fo); if(!IsBadWritePtr(fp,4)) *fp=C().fovValue; }
    if(!g_enabled) return;
    // forward direction + look distance
    float D[3]={tgt[0]-pos[0],tgt[1]-pos[1],tgt[2]-pos[2]};
    float dist=sqrtf(D[0]*D[0]+D[1]*D[1]+D[2]*D[2]);
   if(dist<1e-3f||dist>1e6f) {
        Log("[CamCheck] 失败：距离异常 dist = %f", dist); 
        return;
    }
    D[0]/=dist;D[1]/=dist;D[2]/=dist;
    float un=sqrtf(U[0]*U[0]+U[1]*U[1]+U[2]*U[2]); 
    if(un<1e-3f) {
        Log("[CamCheck] 失败：Up向量长度不正确 un = %f", un); // 💡 加这行
        return;
    }
    float Un[3]={U[0]/un,U[1]/un,U[2]/un};
    // right = D x up (unit)
    float R[3]={D[1]*Un[2]-D[2]*Un[1], D[2]*Un[0]-D[0]*Un[2], D[0]*Un[1]-D[1]*Un[0]};
    float rl=sqrtf(R[0]*R[0]+R[1]*R[1]+R[2]*R[2]); if(rl<1e-3f) {
        Log("[CamCheck] 失败：Right向量长度不正确 rl = %f", rl); // 💡 加这行
        return;
    } R[0]/=rl;R[1]/=rl;R[2]/=rl;
    if(C().camRotEnabled){ const float D2R=0.01745329252f;
        float yaw=g_headYaw*C().camRotScale*D2R, pit=g_headPitch*C().camRotScale*D2R, rol=g_headRoll*C().camRotScale*D2R;
        if(C().matrixTranspose){yaw=-yaw;pit=-pit;}
        auto rot2=[](float*a,float*b,float ang){ float c=cosf(ang),s=sinf(ang);
            for(int i=0;i<3;i++){ float x=a[i],y=b[i]; a[i]=c*x+s*y; b[i]=-s*x+c*y; } };
        rot2(D,R,yaw);   // yaw about up
        rot2(D,Un,pit);  // pitch about right
        rot2(R,Un,rol);  // roll about forward
    }
    // ---- stereo convergence: toe-in rotation ----
    if(Gbfr3D::Stereo_AfrActive()){
        float sgn = (float)Gbfr3D::Stereo_CurrentEyeSign();
        float sep = Gbfr3D::Stereo_SepPerEyeUnits();
        float convDist = Gbfr3D::Stereo_ConvergenceDistance();
        if(convDist > 0.001f && sep > 0.f){
            float convAngle = atan2f(sep, convDist);
            float yawConv = sgn * convAngle;
            float c = cosf(yawConv), s = sinf(yawConv);
            float crossUD[3] = {Un[1]*D[2]-Un[2]*D[1], Un[2]*D[0]-Un[0]*D[2], Un[0]*D[1]-Un[1]*D[0]};
            for(int i=0;i<3;i++) D[i] = c*D[i] + s*crossUD[i];
            R[0]=D[1]*Un[2]-D[2]*Un[1]; R[1]=D[2]*Un[0]-D[0]*Un[2]; R[2]=D[0]*Un[1]-D[1]*Un[0];
            float rl2=sqrtf(R[0]*R[0]+R[1]*R[1]+R[2]*R[2]);
            if(rl2>1e-3f){ R[0]/=rl2; R[1]/=rl2; R[2]/=rl2; }
        }
    }
    float lean[3]={0,0,0};
    if(C().camPosEnabled)
        for(int i=0;i<3;i++) lean[i]=g_lean[0]*R[i]+g_lean[2]*Un[i]+g_lean[1]*D[i];
    // ---- stereo eye offset (AFR) ----
    if(Gbfr3D::Stereo_AfrActive()){
        float sgn = (float)Gbfr3D::Stereo_CurrentEyeSign(); // -1 left / +1 right
        float sep = Gbfr3D::Stereo_SepPerEyeUnits();
        for(int i=0;i<3;i++) lean[i] += sgn * sep * R[i];
    }
    // translate the whole camera (pos AND target) so the view shifts; rebuild target from rotated dir
    for(int i=0;i<3;i++){ pos[i]+=lean[i]; tgt[i]=pos[i]+D[i]*dist; U[i]=Un[i]; }
}

// ------------------------------------------------------- pattern scan / hook ----
static uint8_t* g_mod=nullptr; static size_t g_modSize=0;
static uint8_t* FindAOB(const char* pat){
    // pat: "0F 28 83 ?? ?? ?? ?? ..."
    uint8_t bytes[64]; int mask[64]; int n=0; const char* p=pat;
    while(*p&&n<64){ while(*p==' ')p++; if(!*p)break;
        if(p[0]=='?'){ mask[n]=0; bytes[n]=0; } else { mask[n]=1; bytes[n]=(uint8_t)strtol(p,nullptr,16);} n++; p+=2; }
    for(size_t i=0;i+n<g_modSize;i++){ int k=0; for(;k<n;k++){ if(mask[k]&&g_mod[i+k]!=bytes[k])break; } if(k==n) return g_mod+i; }
    return nullptr;
}
static uint8_t* g_hook=nullptr; static uint8_t* g_cave=nullptr; static uint8_t g_orig[23];
static bool InstallCameraHook(){
    // CAMERA_WRITE1: vmovaps [rsi+0x10],xmm0 ; vmovaps [rsi+0x20],xmm1 ; vmovaps xmm0,[rsi+0x120] ; vmovaps [rsi+0x30],xmm0
    const char* AOB="C5 F8 29 46 ?? C5 F8 29 4E ?? C5 F8 28 86 ?? ?? ?? ?? C5 F8 29 46 ??";
    uint8_t* addr=FindAOB(AOB);
    if(!addr){ Log("camera: CAMERA_WRITE1 AOB not found"); return false; }
    g_hook=addr; memcpy(g_orig,addr,23);
    g_posOff=addr[4]; g_fwdOff=addr[9]; g_upOff=addr[22];
    g_cave=(uint8_t*)VirtualAlloc(nullptr,256,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    if(!g_cave){ Log("camera: cave alloc failed"); return false; }
    uint8_t* c=g_cave; auto B=[&](uint8_t v){*c++=v;}; auto Q=[&](uint64_t v){memcpy(c,&v,8);c+=8;};
    // 1) re-emit the 4 stolen writes (game's base camera) -- all [rsi+disp], safe to copy
    for(int i=0;i<23;i++) B(g_orig[i]);
    // 2) call ApplyHeadToGBFRCamera(rsi)
    B(0x55); B(0x50);B(0x51);B(0x52); B(0x41);B(0x50);B(0x41);B(0x51);B(0x41);B(0x52);B(0x41);B(0x53); // push rbp,rax,rcx,rdx,r8-11
    B(0x48);B(0x89);B(0xE5); B(0x48);B(0x83);B(0xE4);B(0xF0); B(0x48);B(0x83);B(0xEC);B(0x20);
    B(0x48);B(0x89);B(0xF1);                        // mov rcx,rsi (camera struct)
    B(0x48);B(0xB8);Q((uint64_t)(uintptr_t)&ApplyHeadToGBFRCamera); B(0xFF);B(0xD0);  // mov rax,imm; call rax
    B(0x48);B(0x89);B(0xEC);
    B(0x41);B(0x5B);B(0x41);B(0x5A);B(0x41);B(0x59);B(0x41);B(0x58); B(0x5A);B(0x59);B(0x58); B(0x5D);
    B(0xFF);B(0x25);B(0x00);B(0x00);B(0x00);B(0x00); Q((uint64_t)(uintptr_t)(g_hook+23));  // jmp back
    // write 14-byte abs jmp + 9 nops over the 23 stolen bytes
    DWORD old; VirtualProtect(g_hook,23,PAGE_EXECUTE_READWRITE,&old);
    uint8_t* h=g_hook; h[0]=0xFF;h[1]=0x25;h[2]=0;h[3]=0;h[4]=0;h[5]=0; uint64_t cv=(uint64_t)(uintptr_t)g_cave; memcpy(h+6,&cv,8);
    for(int i=14;i<23;i++) h[i]=0x90;
    VirtualProtect(g_hook,23,old,&old); FlushInstructionCache(GetCurrentProcess(),g_hook,23);
    Log("camera: CAMERA_WRITE1 hook @ module+%llX (pos@+0x%X lookAt@+0x%X up@+0x%X, read from code)",(unsigned long long)(g_hook-g_mod),g_posOff,g_fwdOff,g_upOff);
    return true;
}
// ------------------------------------------------------------ math / smooth ----
static inline float clampf(float v,float lo,float hi){return v<lo?lo:(v>hi?hi:v);}
static void ResetSmooth(){ s_hy=s_hp=s_hr=s_lx=s_ly=s_lz=0; s_vy=s_vp=s_vr=s_vx=s_vyv=s_vz=0; }
struct HD{ float hy,hp,hr,lx,ly,lz; };
static HD Wobble(float dt){ s_wobbleT+=dt; HD d{0,0,0,0,0,0};
    if(g.wobbleMode==0){ float s=sinf((float)s_wobbleT*g.wobbleHz*2*PI)*g.wobbleDeg;
        if(g.wobbleAxis==0)d.hy=s; else if(g.wobbleAxis==1)d.hp=s; else d.hr=s; }
    else{ const float seg=2.5f; int idx=((int)(s_wobbleT/seg))%6; float ph=(float)fmod(s_wobbleT,seg)/seg; float s=sinf(ph*2*PI);
        switch(idx){case 0:d.hy=s*g.wobbleDeg;break;case 1:d.hp=s*g.wobbleDeg;break;case 2:d.hr=s*g.wobbleDeg;break;
            case 3:d.lx=s*g.wobbleLean;break;case 4:d.ly=s*g.wobbleLean;break;case 5:d.lz=s*g.wobbleLean;break;} }
    d.hy*=(g.invYaw?-1.f:1.f);d.hp*=(g.invPitch?-1.f:1.f);d.hr*=(g.invRoll?-1.f:1.f);
    d.lx*=(g.invX?-1.f:1.f);d.ly*=(g.invY?-1.f:1.f);d.lz*=(g.invZ?-1.f:1.f); return d; }
static HD Track(const Pose& r){ if(!g_haveZero){g_zero=r;g_haveZero=true;}
    float dy=r.yaw-g_zero.yaw,dp=r.pitch-g_zero.pitch,dr=r.roll-g_zero.roll,dx=r.x-g_zero.x,dyv=r.y-g_zero.y,dz=r.z-g_zero.z; HD d;
    d.hy=clampf(dy*g.yawSens*g.globalScale,-g.limitDeg,g.limitDeg)*(g.invYaw?-1:1);
    d.hp=clampf(dp*g.pitchSens*g.globalScale,-g.limitDeg,g.limitDeg)*(g.invPitch?-1:1);
    d.hr=clampf(dr*g.rollSens*g.globalScale,-g.limitDeg,g.limitDeg)*(g.invRoll?-1:1);
    if(g.swapYZ){float t=dyv;dyv=dz;dz=t;}
    d.lx=clampf(dx*g.leanScaleX*g.leanScale,-g.leanLimit,g.leanLimit)*(g.invX?-1:1);
    d.ly=clampf(dyv*g.leanScaleY*g.leanScale,-g.leanLimit,g.leanLimit)*(g.invY?-1:1);
    d.lz=clampf(dz*g.leanScaleZ*g.leanScale,-g.leanLimit,g.leanLimit)*(g.invZ?-1:1); return d; }
static HD Smooth(HD t,float dt){ if(g.smoothMode==1){ float w=2*PI*clampf(g.smoothHz,0.1f,30.f),et=expf(-w*dt);
        auto st=[&](float&x,float&v,float tg){float c1=x-tg,c2=v+w*c1;x=tg+(c1+c2*dt)*et;v=(c2-w*(c1+c2*dt))*et;};
        st(s_hy,s_vy,t.hy);st(s_hp,s_vp,t.hp);st(s_hr,s_vr,t.hr);st(s_lx,s_vx,t.lx);st(s_ly,s_vyv,t.ly);st(s_lz,s_vz,t.lz);}
    else{float a=clampf(g.smoothEMA,0.01f,1.f);s_hy+=(t.hy-s_hy)*a;s_hp+=(t.hp-s_hp)*a;s_hr+=(t.hr-s_hr)*a;s_lx+=(t.lx-s_lx)*a;s_ly+=(t.ly-s_ly)*a;s_lz+=(t.lz-s_lz)*a;}
    return HD{s_hy,s_hp,s_hr,s_lx,s_ly,s_lz}; }

static void Update(){ long long now=(long long)GetTickCount64(); float dt=s_lastMs?(now-s_lastMs)/1000.f:0.016f; if(dt<=0)dt=0.016f; if(dt>0.1f)dt=0.1f; s_lastMs=now;
    if(g_toggleReq.exchange(false)){g_enabled=!g_enabled;Log("head-tracking %s",g_enabled?"ENABLED":"DISABLED"); if(!g_enabled){g_lean[0]=g_lean[1]=g_lean[2]=0;g_headYaw=g_headPitch=g_headRoll=0;}}
    if(g_recenterReq.exchange(false)){g_haveZero=false;Log("recenter");}
    if(!g_enabled)return;
    HD tgt = g.wobbleTest?Wobble(dt):(g_recv.IsReceiving()?Track(g_recv.Latest()):HD{0,0,0,0,0,0});
    HD h=Smooth(tgt,dt);
    g_headYaw=h.hy;g_headPitch=h.hp;g_headRoll=h.hr; g_lean[0]=h.lx;g_lean[1]=h.lz;g_lean[2]=h.ly;
    static int beat=0; if(g.diagnostics&&(++beat%300)==0) Log("tick: head(y%.1f p%.1f r%.1f lean %.2f/%.2f/%.2f) recv=%d",h.hy,h.hp,h.hr,h.lx,h.ly,h.lz,(int)g_recv.IsReceiving());
}

static DWORD WINAPI MainThread(LPVOID){
    Sleep(2500);
    Log("=== GBFR 3D-6DOF (version.dll) loading ===");
    MODULEINFO mi; HMODULE hm=GetModuleHandleA(nullptr); GetModuleInformation(GetCurrentProcess(),hm,&mi,sizeof(mi));
    g_mod=(uint8_t*)mi.lpBaseOfDll; g_modSize=mi.SizeOfImage;
    LoadConfig();
    if(!g_recv.Start((uint16_t)g.port)) Log("WARN: receiver failed on %d",g.port);
    // AOB may not be ready until the game is fully up; retry
    int tries=0; while(!InstallCameraHook() && tries<120){ Sleep(500); tries++; }
    g_enabled = g.autoEnable?true:false; ResetSmooth(); s_lastMs=(long long)GetTickCount64();
    Log("GBFR-HeadTracking active. Wobble self-test %s.", g.wobbleTest?"ON":"off");
    // ---- init stereo 3D ----
    Gbfr3D::Stereo_Init(IniPath().c_str());
    while(true){
        if(g.hotkeysEnabled){ static bool tp=false,rp=false;
            bool t=(GetAsyncKeyState(g.toggleKey)&0x8000)!=0,r=(GetAsyncKeyState(g.recenterKey)&0x8000)!=0;
            if(t&&!tp)g_toggleReq=true; if(r&&!rp)g_recenterReq=true; tp=t; rp=r; }
        if(g.fovEnabled){ static bool uP=false,dP=false;
            bool u=(GetAsyncKeyState(VK_PRIOR)&0x8000)!=0, dn=(GetAsyncKeyState(VK_NEXT)&0x8000)!=0;
            if(u&&!uP){ g.fovValue-=0.03f; if(g.fovValue<0.10f)g.fovValue=0.10f; Log("FOV -> %.3f",g.fovValue);} 
            if(dn&&!dP){ g.fovValue+=0.03f; if(g.fovValue>3.0f)g.fovValue=3.0f; Log("FOV -> %.3f",g.fovValue);} uP=u; dP=dn; }
        // ---- stereo hotkeys ----
        Gbfr3D::Stereo_PollHotkeys();
        Update(); Sleep(4);
    }
}
BOOL APIENTRY DllMain(HMODULE h,DWORD r,LPVOID){
    if(r==DLL_PROCESS_ATTACH){ DisableThreadLibraryCalls(h); SetSelfModule(h); CreateThread(nullptr,0,MainThread,nullptr,0,nullptr);}
    else if(r==DLL_PROCESS_DETACH){ Gbfr3D::Stereo_Shutdown(); }
    return TRUE;
}
