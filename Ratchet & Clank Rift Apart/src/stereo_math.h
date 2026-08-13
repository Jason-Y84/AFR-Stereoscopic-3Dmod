#pragma once
// ============================================================================
//  Rift Apart 3D+6DOF - Stereo math helpers (inline, header-only)
//  Separation, Convergence HIT, WUPM conversions
// ============================================================================

namespace StereoMath {

// Per-eye separation in world units
// IPD_mm -> half-IPD in metres -> world units via WUPM
static inline float SepPerEye(float separationMM, float wupm) {
    if (separationMM <= 0.f) return 0.f;
    return separationMM * 0.0005f * wupm;  // 0.0005 = (1/1000)/2
}

// Convergence HIT in UV coordinates
// This folds P00/FOV via convergenceScale
static inline float AfrConv(float convergenceScale, float sepPerEye, float convergenceDistance) {
    if (convergenceDistance <= 0.f || sepPerEye <= 0.f) return 0.f;
    return convergenceScale * (sepPerEye / convergenceDistance);
}

} // namespace StereoMath
