// Example HLSL changes for Eye Tracking Foveation
// Add to FoveatedMask.hlsli or FoveatedRender.hlsli

/*
===========================
ADDING EYE TRACKING TO HLSL
===========================

This shows how to update your HLSL shaders to use eye tracking
gaze offsets for dynamic foveation center.

*/

// ============================================================================
// CB_DYNAMIC (Constant Buffer) - Add these fields
// ============================================================================
// Existing field:
// float4 FoveationCenterOffsets;  // xy=left eye, zw=right eye

// This already exists in your struct! Just update values from C++ side.
// In C++: foveationProfile.centerOffsets[0] and [1] → set this cbuffer


// ============================================================================
// Main Shader Code Changes
// ============================================================================

/**
 * Get foveation center adjusted for eye tracking
 * 
 * @param eyeIndex 0=left, 1=right
 * @param foveationCenterOffsets Offset from eye tracking (pixels)
 * @return Center in NDC space (-1..1)
 */
float2 GetFoveationCenterWithEyeTracking(
    uint eyeIndex,
    float2 foveationCenterOffsets)
{
    // Base center is always (0, 0) in the middle of the eye
    float2 center = float2(0.0, 0.0);

    // Apply eye tracking offset
    // Convert from pixel space to NDC space
    // For VR SBS: half eye width = 960 pixels = 1.0 NDC
    const float PIXELS_TO_NDC = 1.0 / 960.0;
    float2 eyeTrackingOffset = foveationCenterOffsets * PIXELS_TO_NDC;

    // Clamp to valid region (don't go too far to edges)
    eyeTrackingOffset = clamp(eyeTrackingOffset, float2(-0.3, -0.3), float2(0.3, 0.3));

    return center + eyeTrackingOffset;
}

/**
 * Calculate foveation mask with dynamic eye tracking center
 * 
 * This replaces the static foveation center with eye tracking-aware center
 */
float4 GetFoveationMaskWithEyeTracking(
    float2 uv,                      // Pixel UV in NDC space (-1..1)
    uint eyeIndex,                  // 0=left, 1=right
    float coverageScale,            // How much of eye is high-res [0..1]
    float2 foveationCenterOffset)   // Eye tracking offset (pixels)
{
    // Get dynamic foveation center based on eye tracking
    float2 foveationCenter = GetFoveationCenterWithEyeTracking(
        eyeIndex, 
        foveationCenterOffset);

    // Distance from foveation center in NDC space
    float2 delta = uv - foveationCenter;
    float distanceFromCenter = length(delta);

    // Superellipse foveation shape (more natural than circle)
    // Adjust these parameters for your foveation profile shape
    float ellipseX = abs(delta.x) / (coverageScale * 0.5);
    float ellipseY = abs(delta.y) / (coverageScale * 0.4);  // Slightly tighter vertical

    // Superellipse: x^4 + y^4
    float ellipseMask = pow(ellipseX, 4.0) + pow(ellipseY, 4.0);

    // Feather at edges for smooth transition
    float FEATHER_WIDTH = 0.1;  // NDC units
    float featheredMask = smoothstep(
        1.0 + FEATHER_WIDTH,
        1.0 - FEATHER_WIDTH,
        ellipseMask);

    return float4(featheredMask, featheredMask, featheredMask, featheredMask);
}


// ============================================================================
// Subrect Stretch with Eye Tracking
// ============================================================================

/**
 * Adjust subrect bounds based on eye tracking offset
 * 
 * The subrect (high-res region) moves with the gaze point
 */
float4 GetEyeTrackingAdjustedSubrect(
    float4 baseSubrect,             // Original subrect [0..1920, 0..1080]
    float2 eyeTrackingOffset,       // Offset from eye tracking (pixels)
    float subrectWidth,             // Width of subrect (pixels, e.g., 960)
    float subrectHeight)            // Height of subrect (pixels, e.g., 540)
{
    // Apply offset to center (clamp to valid region)
    float2 offsetClamped = clamp(
        eyeTrackingOffset,
        float2(-subrectWidth * 0.25),
        float2(subrectWidth * 0.25));

    // New subrect bounds
    float4 adjustedSubrect = baseSubrect;
    adjustedSubrect.x = baseSubrect.x + offsetClamped.x;
    adjustedSubrect.y = baseSubrect.y + offsetClamped.y;

    return adjustedSubrect;
}


// ============================================================================
// Scratch implementation of existing mask but with eye tracking
// ============================================================================

// In your existing FoveatedMaskCS or FoveatedMaskPS shader:

// Before:
// float4 mask = GetFoveationMask(uv, coverageScale);

// After (with eye tracking):
// Get eye index from SV_POSITION or ISA
// uint eyeIndex = GetEyeIndex(positionSS);  
// float2 eyeTrackingOffset = GetEyeTrackingOffset(eyeIndex);  // From cbuffer
// float4 mask = GetFoveationMaskWithEyeTracking(uv, eyeIndex, coverageScale, eyeTrackingOffset);


// ============================================================================
// C++ Side: How to pass data to shaders
// ============================================================================

/*

// In your constant buffer struct (from C++):
struct FoveationDataCB
{
    float4 foveationCenterOffsets;  // xy=left eye offset, zw=right eye offset (PIXELS)
    float coverageScale;
    float centerHorizontalScale;
    uint debugVisualize;
    uint padding;
};

// Update this in C++ each frame:
FoveationDataCB fovData;
fovData.foveationCenterOffsets = float4(
    profile.centerOffsets[0].x,  // Left eye X offset
    profile.centerOffsets[0].y,  // Left eye Y offset
    profile.centerOffsets[1].x,  // Right eye X offset
    profile.centerOffsets[1].y   // Right eye Y offset
);
fovData.coverageScale = profile.coverageScale;

// Update constant buffer and bind to shaders...

*/

