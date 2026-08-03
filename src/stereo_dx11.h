// stereo_dx11.h - DX11 AFR stereo for GBFR
#pragma once
#include <cstdint>

namespace Gbfr3D {

bool  Stereo_Init(const char* iniPath);
void  Stereo_Shutdown();
void  Stereo_PollHotkeys();

// Called from the camera hook (dllmain.cpp):
bool  Stereo_AfrActive();
int   Stereo_CurrentEyeSign();     // -1 left / +1 right
float Stereo_SepPerEyeUnits();
float Stereo_ConvergenceDistance();
float Stereo_WorldUnitsPerMetre();
unsigned Stereo_FrameIndex();
int   Stereo_FrameEye();
void  Stereo_ReportCameraEye(int eye);
void  Stereo_NoteCameraWrite();
void  Stereo_PresentEyeFallback();  // feed eye clock from Present when camera hook silent

} // namespace Gbfr3D
