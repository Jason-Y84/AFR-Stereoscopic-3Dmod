// Self-contained version.dll proxy: forwards all exports to the real system version.dll.
// Internal names are vproxy_*; the .def aliases them to the real export names to avoid
// clashing with the prototypes in <windows.h>.
#include <windows.h>
typedef void*(WINAPI* genfn)(void*,void*,void*,void*,void*,void*);
static HMODULE g_real=nullptr;
static genfn g_p[17]={0};
static void EnsureReal(){
    if(g_real) return;
    char p[MAX_PATH]; GetSystemDirectoryA(p,MAX_PATH); lstrcatA(p,"\\version.dll");
    g_real=LoadLibraryA(p); if(!g_real) return;
    g_p[0]=(genfn)GetProcAddress(g_real,"GetFileVersionInfoA");
    g_p[1]=(genfn)GetProcAddress(g_real,"GetFileVersionInfoByHandle");
    g_p[2]=(genfn)GetProcAddress(g_real,"GetFileVersionInfoExA");
    g_p[3]=(genfn)GetProcAddress(g_real,"GetFileVersionInfoExW");
    g_p[4]=(genfn)GetProcAddress(g_real,"GetFileVersionInfoSizeA");
    g_p[5]=(genfn)GetProcAddress(g_real,"GetFileVersionInfoSizeExA");
    g_p[6]=(genfn)GetProcAddress(g_real,"GetFileVersionInfoSizeExW");
    g_p[7]=(genfn)GetProcAddress(g_real,"GetFileVersionInfoSizeW");
    g_p[8]=(genfn)GetProcAddress(g_real,"GetFileVersionInfoW");
    g_p[9]=(genfn)GetProcAddress(g_real,"VerFindFileA");
    g_p[10]=(genfn)GetProcAddress(g_real,"VerFindFileW");
    g_p[11]=(genfn)GetProcAddress(g_real,"VerInstallFileA");
    g_p[12]=(genfn)GetProcAddress(g_real,"VerInstallFileW");
    g_p[13]=(genfn)GetProcAddress(g_real,"VerLanguageNameA");
    g_p[14]=(genfn)GetProcAddress(g_real,"VerLanguageNameW");
    g_p[15]=(genfn)GetProcAddress(g_real,"VerQueryValueA");
    g_p[16]=(genfn)GetProcAddress(g_real,"VerQueryValueW");
}
extern "C" void InitVersionProxy(){ EnsureReal(); }
extern "C" void* WINAPI vproxy_GetFileVersionInfoA(void*a,void*b,void*c,void*d,void*e2,void*f){ EnsureReal(); return g_p[0]?g_p[0](a,b,c,d,e2,f):0; }
extern "C" void* WINAPI vproxy_GetFileVersionInfoByHandle(void*a,void*b,void*c,void*d,void*e2,void*f){ EnsureReal(); return g_p[1]?g_p[1](a,b,c,d,e2,f):0; }
extern "C" void* WINAPI vproxy_GetFileVersionInfoExA(void*a,void*b,void*c,void*d,void*e2,void*f){ EnsureReal(); return g_p[2]?g_p[2](a,b,c,d,e2,f):0; }
extern "C" void* WINAPI vproxy_GetFileVersionInfoExW(void*a,void*b,void*c,void*d,void*e2,void*f){ EnsureReal(); return g_p[3]?g_p[3](a,b,c,d,e2,f):0; }
extern "C" void* WINAPI vproxy_GetFileVersionInfoSizeA(void*a,void*b,void*c,void*d,void*e2,void*f){ EnsureReal(); return g_p[4]?g_p[4](a,b,c,d,e2,f):0; }
extern "C" void* WINAPI vproxy_GetFileVersionInfoSizeExA(void*a,void*b,void*c,void*d,void*e2,void*f){ EnsureReal(); return g_p[5]?g_p[5](a,b,c,d,e2,f):0; }
extern "C" void* WINAPI vproxy_GetFileVersionInfoSizeExW(void*a,void*b,void*c,void*d,void*e2,void*f){ EnsureReal(); return g_p[6]?g_p[6](a,b,c,d,e2,f):0; }
extern "C" void* WINAPI vproxy_GetFileVersionInfoSizeW(void*a,void*b,void*c,void*d,void*e2,void*f){ EnsureReal(); return g_p[7]?g_p[7](a,b,c,d,e2,f):0; }
extern "C" void* WINAPI vproxy_GetFileVersionInfoW(void*a,void*b,void*c,void*d,void*e2,void*f){ EnsureReal(); return g_p[8]?g_p[8](a,b,c,d,e2,f):0; }
extern "C" void* WINAPI vproxy_VerFindFileA(void*a,void*b,void*c,void*d,void*e2,void*f){ EnsureReal(); return g_p[9]?g_p[9](a,b,c,d,e2,f):0; }
extern "C" void* WINAPI vproxy_VerFindFileW(void*a,void*b,void*c,void*d,void*e2,void*f){ EnsureReal(); return g_p[10]?g_p[10](a,b,c,d,e2,f):0; }
extern "C" void* WINAPI vproxy_VerInstallFileA(void*a,void*b,void*c,void*d,void*e2,void*f){ EnsureReal(); return g_p[11]?g_p[11](a,b,c,d,e2,f):0; }
extern "C" void* WINAPI vproxy_VerInstallFileW(void*a,void*b,void*c,void*d,void*e2,void*f){ EnsureReal(); return g_p[12]?g_p[12](a,b,c,d,e2,f):0; }
extern "C" void* WINAPI vproxy_VerLanguageNameA(void*a,void*b,void*c,void*d,void*e2,void*f){ EnsureReal(); return g_p[13]?g_p[13](a,b,c,d,e2,f):0; }
extern "C" void* WINAPI vproxy_VerLanguageNameW(void*a,void*b,void*c,void*d,void*e2,void*f){ EnsureReal(); return g_p[14]?g_p[14](a,b,c,d,e2,f):0; }
extern "C" void* WINAPI vproxy_VerQueryValueA(void*a,void*b,void*c,void*d,void*e2,void*f){ EnsureReal(); return g_p[15]?g_p[15](a,b,c,d,e2,f):0; }
extern "C" void* WINAPI vproxy_VerQueryValueW(void*a,void*b,void*c,void*d,void*e2,void*f){ EnsureReal(); return g_p[16]?g_p[16](a,b,c,d,e2,f):0; }
