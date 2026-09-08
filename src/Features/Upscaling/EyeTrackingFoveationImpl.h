// Example Integration of Eye Tracking Foveation into FoveatedRender
// Add this to your FoveatedRender.cpp rendering pipeline

/*
=== HOW TO INTEGRATE INTO EXISTING CODE ===

1. In FoveatedRender.cpp, in the main rendering function where GetFoveationProfile() is called:

	// Get foveation profile (existing code)
	auto foveationProfile = GetFoveationProfile();

	// NEW: Apply eye tracking offset if enabled
	if (settings.eyeTrackingFoveationEnabled && globals::game::isVR)
	{
		ApplyEyeTrackingOffsetToFoveation(foveationProfile);
	}

2. The function below handles all the logic needed to move the foveation center
   based on eye tracking data while maintaining stability and performance.

*/

#pragma once

#include "FoveatedRender.h"
#include "EyeTrackingFoveation.h"
#include "../VR/EyeTrackingVR.h"
#include <SimpleMath.h>

namespace FoveatedRenderImpl
{
	/**
	 * @brief Apply eye tracking gaze offset to foveation center
	 * 
	 * This function updates the foveation center offset based on current
	 * eye gaze position. It handles:
	 * - Temporal smoothing to reduce tracker jitter
	 * - Deadzone filtering to prevent noise
	 * - Clamping to valid regions
	 * - Per-eye offsets for stereo rendering
	 */
	inline void ApplyEyeTrackingOffsetToFoveation(
		FoveatedRender::FoveationProfile& inoutProfile,
		const FoveatedRender::Settings& settings,
		FoveatedRenderEyeTracking::EyeTrackingData& inoutEyeTracking)
	{
		// Try to get current eye tracking data
		auto eyeTrackingVR = TryGetEyeTrackingData();

		if (!eyeTrackingVR.isValid || eyeTrackingVR.confidence < settings.gazeConfidenceThreshold)
		{
			// Eye tracking failed or confidence too low - use last valid position
			return;
		}

		// Update left eye gaze with temporal smoothing
		if (FoveatedRenderEyeTracking::HasSignificantGazeMovement(
			eyeTrackingVR.gazeNDCLeft,
			inoutEyeTracking.smoothedGazeLeft,
			settings.eyeTrackingGazeThresholdPixels))
		{
			inoutEyeTracking.smoothedGazeLeft = FoveatedRenderEyeTracking::SmoothGazePoint(
				eyeTrackingVR.gazeNDCLeft,
				inoutEyeTracking.smoothedGazeLeft,
				settings.eyeTrackingGazeSmoothingAlpha);
			inoutEyeTracking.gazeNDCLeft = eyeTrackingVR.gazeNDCLeft;
		}

		// Update right eye gaze with temporal smoothing
		if (FoveatedRenderEyeTracking::HasSignificantGazeMovement(
			eyeTrackingVR.gazeNDCRight,
			inoutEyeTracking.smoothedGazeRight,
			settings.eyeTrackingGazeThresholdPixels))
		{
			inoutEyeTracking.smoothedGazeRight = FoveatedRenderEyeTracking::SmoothGazePoint(
				eyeTrackingVR.gazeNDCRight,
				inoutEyeTracking.smoothedGazeRight,
				settings.eyeTrackingGazeSmoothingAlpha);
			inoutEyeTracking.gazeNDCRight = eyeTrackingVR.gazeNDCRight;
		}

		// Convert gaze points to subrect offsets
		constexpr float MAX_SUBRECT_OFFSET_PIXELS = 150.0f;

		auto offsetLeft = FoveatedRenderEyeTracking::GazeToSubrectOffset(
			inoutEyeTracking.smoothedGazeLeft,
			MAX_SUBRECT_OFFSET_PIXELS);

		auto offsetRight = FoveatedRenderEyeTracking::GazeToSubrectOffset(
			inoutEyeTracking.smoothedGazeRight,
			MAX_SUBRECT_OFFSET_PIXELS);

		// Update foveation profile center offsets
		// centerOffsets[0] = left eye, centerOffsets[1] = right eye
		inoutProfile.centerOffsets[0] = offsetLeft;
		inoutProfile.centerOffsets[1] = offsetRight;

		// Mark that eye tracking is active (optional, for debugging)
		inoutProfile.available = true;
	}

	/**
	 * @brief Simple wrapper for toggling eye tracking foveation
	 * 
	 * Call from ImGui settings menu to enable/disable dynamic foveation
	 */
	inline void SetEyeTrackingFoveationEnabled(
		FoveatedRender::Settings& settings,
		bool enabled)
	{
		settings.eyeTrackingFoveationEnabled = enabled ? 1 : 0;
	}

	/**
	 * @brief Adjust eye tracking smoothing parameter
	 * 
	 * Higher values = more smoothing (less jittery but slower response)
	 * Lower values = less smoothing (responsive but may feel jittery)
	 * 
	 * Recommended range: 0.1 - 0.4
	 */
	inline void SetEyeTrackingSmoothing(
		FoveatedRender::Settings& settings,
		float alpha)
	{
		settings.eyeTrackingGazeSmoothingAlpha = DirectX::SimpleMath::Clamp(alpha, 0.0f, 1.0f);
	}

}  // namespace FoveatedRenderImpl

/*
=== HLSL SHADER INTEGRATION ===

In your FoveatedMask.hlsli or subrect shader, use the new centerOffsets:

	float2 GetFoveationCenterNDC(uint eyeIndex)
	{
		// Use the eye-tracking-adjusted center offset
		// This moves the foveation mask based on gaze point

		// Original center (0, 0)
		float2 center = float2(0.0, 0.0);

		// Apply eye tracking offset
		// Convert from pixel space back to NDC if needed
		const float PIXELS_TO_NDC = 1.0 / 960.0;  // Half-res eye width
		float2 eyeTrackingOffset = centerOffsets[eyeIndex] * PIXELS_TO_NDC;

		return center + eyeTrackingOffset;
	}

Then in the mask calculation:
	float2 foveationCenter = GetFoveationCenterNDC(eyeIndex);
	float4 mask = GetFoveationMask(uv, foveationCenter, coverageScale);

*/
