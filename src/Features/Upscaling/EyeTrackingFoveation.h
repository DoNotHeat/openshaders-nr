// Eye Tracking Foveation Extension for FoveatedRender
// Implements dynamic foveation center following user gaze

#pragma once

#include <cmath>

namespace FoveatedRenderEyeTracking
{
	/**
	 * @brief Eye Tracking Data - contains current gaze information from VR headset
	 *
	 * Used for dynamic foveation center following the user's gaze point.
	 * Values are updated every frame from the VR runtime.
	 */
	struct EyeTrackingData
	{
		bool isValid = false;             // true if eye tracking data is available
		float gazeNDCLeft[2] = { 0.0f, 0.0f };    // Gaze point in NDC space for left eye (-1..1)
		float gazeNDCRight[2] = { 0.0f, 0.0f };   // Gaze point in NDC space for right eye (-1..1)
		float smoothedGazeLeft[2] = { 0.0f, 0.0f };   // Temporal smoothed gaze point (left)
		float smoothedGazeRight[2] = { 0.0f, 0.0f };  // Temporal smoothed gaze point (right)
	};

	/**
	 * @brief Updates eye tracking gaze point with temporal smoothing
	 *
	 * Applies exponential moving average filtering to reduce jitter from
	 * eye tracker noise while maintaining responsiveness.
	 *
	 * @param currentGaze New gaze position from eye tracker (NDC space -1..1)
	 * @param previousSmoothed Previous smoothed gaze position
	 * @param alpha Smoothing factor [0..1] (higher = more smoothing)
	 * @return Smoothed gaze position
	 */
	inline void SmoothGazePoint(
		const float* currentGaze,
		const float* previousSmoothed,
		float alpha,
		float* outSmoothed)
	{
		// Exponential moving average: smoothed = (1-alpha)*previous + alpha*current
		outSmoothed[0] = previousSmoothed[0] + alpha * (currentGaze[0] - previousSmoothed[0]);
		outSmoothed[1] = previousSmoothed[1] + alpha * (currentGaze[1] - previousSmoothed[1]);
	}

	/**
	 * @brief Applies deadzone filtering to gaze movement
	 *
	 * Prevents small jittery movements from updating the foveation center
	 * by requiring a minimum pixel movement threshold.
	 *
	 * @param gazeNDC Current gaze position in NDC space (-1..1)
	 * @param previousGazeNDC Previous gaze position in NDC space
	 * @param thresholdPixels Minimum pixel movement in half-screen space
	 * @return true if movement exceeds threshold, false if within deadzone
	 */
	inline bool HasSignificantGazeMovement(
		const float* gazeNDC,
		const float* previousGazeNDC,
		float thresholdPixels)
	{
		// Convert NDC offset to pixel space (assume 1920x1080 half-res VR eye)
		// NDC range -1..1 maps to 0..1920 (half width for one eye in SBS)
		constexpr float pixelsPerNDCUnit = 960.0f;  // 1920 / 2

		const float dx = (gazeNDC[0] - previousGazeNDC[0]) * pixelsPerNDCUnit;
		const float dy = (gazeNDC[1] - previousGazeNDC[1]) * pixelsPerNDCUnit;

		return std::sqrt(dx * dx + dy * dy) > thresholdPixels;
	}

	/**
	 * @brief Clamps gaze point to center region of eye
	 *
	 * Prevents foveation center from being pushed too far to the edges
	 * where rendering geometry may not exist.
	 *
	 * @param gazeNDC Gaze position in NDC space (-1..1)
	 * @param maxOffsetNDC Maximum offset from center (e.g., 0.4 = 80% of eye width)
	 * @param outClamped Receives the clamped gaze position
	 */
	inline void ClampGazeToValidRegion(
		const float* gazeNDC,
		float maxOffsetNDC,
		float* outClamped)
	{
		auto clamp1 = [maxOffsetNDC](float v) {
			return v < -maxOffsetNDC ? -maxOffsetNDC : (v > maxOffsetNDC ? maxOffsetNDC : v);
		};
		outClamped[0] = clamp1(gazeNDC[0]);
		outClamped[1] = clamp1(gazeNDC[1]);
	}

	/**
	 * @brief Converts eye tracking gaze to subrect center offset
	 *
	 * Maps normalized gaze point to the offset that should be applied
	 * to move the foveation center.
	 *
	 * @param gazeNDC Gaze position in NDC space (-1..1)
	 * @param maxSubrectOffsetPixels Maximum offset for subrect center (e.g., 100 pixels)
	 * @param outOffset Receives the offset in pixel coordinates
	 */
	inline void GazeToSubrectOffset(
		const float* gazeNDC,
		float maxSubrectOffsetPixels,
		float* outOffset)
	{
		// Clamp gaze to valid region first
		float clamped[2];
		ClampGazeToValidRegion(gazeNDC, 0.4f, clamped);

		// Map NDC (-0.4..0.4) to pixel offset
		const float pixelsPerNDCUnit = maxSubrectOffsetPixels / 0.4f;

		outOffset[0] = clamped[0] * pixelsPerNDCUnit;
		outOffset[1] = clamped[1] * pixelsPerNDCUnit;
	}

}  // namespace FoveatedRenderEyeTracking
