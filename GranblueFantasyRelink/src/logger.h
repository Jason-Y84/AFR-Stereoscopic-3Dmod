#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace P5HT {

inline HMODULE& SelfModule(){ static HMODULE h=nullptr; return h; }
inline void SetSelfModule(HMODULE h){ SelfModule()=h; }

inline void SelfDir(wchar_t* out){
    if(!GetModuleFileNameW(SelfModule(),out,MAX_PATH))
        if(!GetModuleFileNameW(nullptr,out,MAX_PATH)){ out[0]=0; return; }
    if(wchar_t* s=wcsrchr(out,L'\\')) *(s+1)=0;
}

// Pick a writable log path among several candidates, using raw Win32 (no CRT).
inline const wchar_t* LogPath(){
    static wchar_t chosen[MAX_PATH]={0}; static bool done=false;
    if(done) return chosen[0]?chosen:nullptr; done=true;
    wchar_t c[5][MAX_PATH]; int n=0;
    if(GetModuleFileNameW(SelfModule(),c[n],MAX_PATH)){ if(wchar_t*s=wcsrchr(c[n],L'\\')){*(s+1)=0; wcscat_s(c[n],MAX_PATH,L"3D-6DOF.log"); n++; } }
    if(GetModuleFileNameW(nullptr,c[n],MAX_PATH)){ if(wchar_t*s=wcsrchr(c[n],L'\\')){*(s+1)=0; wcscat_s(c[n],MAX_PATH,L"3D-6DOF.log"); n++; } }
    { wchar_t t[MAX_PATH]; if(GetTempPathW(MAX_PATH,t)){ wcscpy_s(c[n],MAX_PATH,t); wcscat_s(c[n],MAX_PATH,L"3D-6DOF.log"); n++; } }
    { if(GetEnvironmentVariableW(L"USERPROFILE",c[n],MAX_PATH)){ wcscat_s(c[n],MAX_PATH,L"\\Desktop\\3D-6DOF.log"); n++; } }
    wcscpy_s(c[n],MAX_PATH,L"C:\\3D-6DOF.log"); n++;
    for(int i=0;i<n;i++){
        HANDLE h=CreateFileW(c[i],FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h!=INVALID_HANDLE_VALUE){ CloseHandle(h); wcscpy_s(chosen,MAX_PATH,c[i]);
            char m[MAX_PATH]; wcstombs(m,chosen,MAX_PATH); OutputDebugStringA("[3D-6DOF] log -> "); OutputDebugStringA(m); OutputDebugStringA("\n"); return chosen; }
    }
    return nullptr;
}

inline void Log(const char* fmt, ...) {
    char buf[1100]; int p=0;
    va_list ap; va_start(ap,fmt); p=vsnprintf(buf,sizeof(buf)-2,fmt,ap); va_end(ap);
    if(p<0) p=0; if(p>(int)sizeof(buf)-2) p=sizeof(buf)-2; buf[p++]='\r'; buf[p++]='\n';
    OutputDebugStringA("[3D-6DOF] ");
    { char t[1102]; memcpy(t,buf,p); t[p]=0; OutputDebugStringA(t); }
    const wchar_t* path=LogPath();
    if(path){
        HANDLE h=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h!=INVALID_HANDLE_VALUE){ DWORD w; SetFilePointer(h,0,nullptr,FILE_END); WriteFile(h,buf,p,&w,nullptr); CloseHandle(h); }
    }
}

} // namespace P5HT
