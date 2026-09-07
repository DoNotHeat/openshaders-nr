// Eye Tracking data acquisition for VR.
// Real source: reads per-eye NDC foveation centers from the SteamVR eye
// tracking component exposed by the Pimax Dream driver (sboys3) via
// IVRSystem::GetEyeTrackedFoveationCenter (OpenVR 2.15.x, IVRSystem_026).
//
// The bundled openvr.h is an older IVRSystem_019 without the eye-tracking
// method, and the engine's own vrSystem is 019. So we fetch a *fresh*
// IVRSystem_026 interface directly from openvr_api.dll and call the method
// through a locally-declared struct whose vtable matches 026. This is
// isolated from CommonLib (which never calls IVRSystem methods through our
// header) and from the engine's 019 vrSystem.
//
// Falls back to a mock Lissajous source when no eye tracker is present, so
// the debug overlay keeps working without hardware.

#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <windows.h>

namespace FoveatedRenderEyeTracking
{
	/**
	 * @brief Per-frame eye tracking sample.
	 *
	 * Gaze points are in per-eye NDC space (-1..1, y up), origin at the eye's
	 * center. Produced by the active gaze source (mock or OpenVR).
	 */
	struct EyeTrackingDataVR
	{
		bool isValid = false;      // true if this sample carries usable gaze data
		float confidence = 0.0f;  // source confidence [0..1]
		float gazeNDCLeft[2] = { 0.0f, 0.0f };   // x, y
		float gazeNDCRight[2] = { 0.0f, 0.0f };  // x, y
	};

	/** @brief Which gaze source produced the last sample. */
	enum class GazeSource
	{
		kNone = 0,  // no data this frame
		kMock = 1,  // synthetic Lissajous path (development)
		kOpenVR = 2  // real headset eye tracker
	};

	// ---------------------------------------------------------------------
	// Minimal OpenVR 2.15.x (IVRSystem_026) declarations needed to call
	// GetEyeTrackedFoveationCenter. We only declare the methods up to and
	// including the one we call, in the exact vtable order of IVRSystem_026,
	// so the virtual slot index is correct. Everything after is irrelevant to
	// us and omitted.
	// ---------------------------------------------------------------------
	namespace vr026
	{
		struct HmdVector2_t
		{
			float v[2];
		};

		class IVRSystem
		{
		public:
			// 0
			virtual void GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight) = 0;
			// 1
			virtual void GetProjectionMatrix(void* eEye, float fNearZ, float fFarZ) = 0;
			// 2
			virtual void GetProjectionRaw(void* eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom) = 0;
			// 3
			virtual bool ComputeDistortion(void* eEye, float fU, float fV, void* pDistortionCoordinates) = 0;
			// 4
			virtual bool ComputeDistortionSet(void* eEye, void* eChannel, bool bAsNormalizedDeviceCoordinates, uint32_t nNumCoordinates, const void* pInput, void* pOutput) = 0;
			// 5
			virtual void GetEyeToHeadTransform(void* eEye) = 0;
			// 6
			virtual bool GetTimeSinceLastVsync(float* pfSecondsSinceLastVsync, uint64_t* pulFrameCounter) = 0;
			// 7
			virtual int32_t GetD3D9AdapterIndex() = 0;
			// 8
			virtual void GetDXGIOutputInfo(int32_t* pnAdapterIndex) = 0;
			// 9
			virtual void GetOutputDevice(uint64_t* pnDevice, void* textureType, void* pInstance = nullptr) = 0;
			// 10
			virtual bool IsDisplayOnDesktop() = 0;
			// 11
			virtual bool SetDisplayVisibility(bool bIsVisibleOnDesktop) = 0;
			// 12
			virtual void GetDeviceToAbsoluteTrackingPose(void* eOrigin, float fPredictedSecondsToPhotonsFromNow, void* pTrackedDevicePoseArray, uint32_t unTrackedDevicePoseArrayCount) = 0;
			// 13
			virtual void GetSeatedZeroPoseToStandingAbsoluteTrackingPose() = 0;
			// 14
			virtual void GetRawZeroPoseToStandingAbsoluteTrackingPose() = 0;
			// 15
			virtual uint32_t GetSortedTrackedDeviceIndicesOfClass(void* eTrackedDeviceClass, void* punTrackedDeviceIndexArray, uint32_t unTrackedDeviceIndexArrayCount, uint32_t unRelativeToTrackedDeviceIndex = 0) = 0;
			// 16
			virtual void* GetTrackedDeviceActivityLevel(uint32_t unDeviceId) = 0;
			// 17
			virtual void ApplyTransform(void* pOutputPose, const void* pTrackedDevicePose, const void* pTransform) = 0;
			// 18
			virtual uint32_t GetTrackedDeviceIndexForControllerRole(void* unDeviceType) = 0;
			// 19
			virtual void* GetControllerRoleForTrackedDeviceIndex(uint32_t unDeviceIndex) = 0;
			// 20
			virtual void* GetTrackedDeviceClass(uint32_t unDeviceIndex) = 0;
			// 21
			virtual bool IsTrackedDeviceConnected(uint32_t unDeviceIndex) = 0;
			// 22
			virtual bool GetBoolTrackedDeviceProperty(uint32_t unDeviceIndex, uint32_t prop, void* pError = nullptr) = 0;
			// 23
			virtual float GetFloatTrackedDeviceProperty(uint32_t unDeviceIndex, uint32_t prop, void* pError = nullptr) = 0;
			// 24
			virtual int32_t GetInt32TrackedDeviceProperty(uint32_t unDeviceIndex, uint32_t prop, void* pError = nullptr) = 0;
			// 25
			virtual uint64_t GetUint64TrackedDeviceProperty(uint32_t unDeviceIndex, uint32_t prop, void* pError = nullptr) = 0;
			// 26
			virtual void GetMatrix34TrackedDeviceProperty(uint32_t unDeviceIndex, uint32_t prop, void* pError = nullptr) = 0;
			// 27
			virtual uint32_t GetArrayTrackedDeviceProperty(uint32_t unDeviceIndex, uint32_t prop, uint32_t propType, void* pBuffer, uint32_t unBufferSize, void* pError = nullptr) = 0;
			// 28
			virtual uint32_t GetStringTrackedDeviceProperty(uint32_t unDeviceIndex, uint32_t prop, char* pchValue, uint32_t unBufferSize, void* pError = nullptr) = 0;
			// 29
			virtual const char* GetPropErrorNameFromEnum(uint32_t error) = 0;
			// 30
			virtual bool PollNextEvent(void* pEvent, uint32_t uncbVREvent) = 0;
			// 31
			virtual bool PollNextEventWithPose(void* eOrigin, void* pEvent, uint32_t uncbVREvent, void* pTrackedDevicePose) = 0;
			// 32
			virtual bool PollNextEventWithPoseAndOverlays(void* eOrigin, void* pEvent, uint32_t uncbVREvent, void* pTrackedDevicePose, uint64_t* pulOverlayHandle) = 0;
			// 33
			virtual const char* GetEventTypeNameFromEnum(uint32_t eType) = 0;
			// 34
			virtual void GetHiddenAreaMesh(void* eEye, void* type = nullptr) = 0;
			// 35
			virtual bool GetEyeTrackedFoveationCenter(HmdVector2_t* pNdcLeft, HmdVector2_t* pNdcRight) = 0;
			// 36
			virtual bool GetEyeTrackedFoveationCenterForProjection(const void* pProjMat, HmdVector2_t* pNdc) = 0;
		};
	}  // namespace vr026

	// ---------------------------------------------------------------------
	// Real OpenVR eye tracking source
	// ---------------------------------------------------------------------

/** @brief Per-process gaze-source state, cached across frames.
 *
 *  Holds the resolved IVRSystem_026 (interface lookup is done once, not per
 *  frame) and the last valid gaze sample so a transient tracker failure can
 *  hold position instead of switching sources mid-stream.
 */
struct GazeSourceState
{
	vr026::IVRSystem* system = nullptr;  // cached IVRSystem_026; null until resolved
	bool everSucceeded = false;          // real tracker produced data at least once
	uint32_t framesSinceSuccess = 0;     // consecutive failed frames since last success
	float lastLeft[2] = { 0.0f, 0.0f };
	float lastRight[2] = { 0.0f, 0.0f };
	GazeSource lastSource = GazeSource::kNone;
};

inline GazeSourceState& GetGazeSourceState()
{
	static GazeSourceState state;
	return state;
}

/** @brief Read the per-eye NDC foveation centers from SteamVR.
 *  Resolves IVRSystem_026 from openvr_api.dll on first use (retried until the
 *  runtime loads the module) and caches it. Returns true on success. */
inline bool TryGetOpenVREyeTracking(float* outLeft, float* outRight)
{
	auto& state = GetGazeSourceState();

	if (!state.system) {
		using pfnVRGetGenericInterface = void* (*)(const char*, int*);

		HMODULE openvr = GetModuleHandleA("openvr_api.dll");
		if (!openvr)
			return false;

		auto VR_GetGenericInterface = (pfnVRGetGenericInterface)GetProcAddress(openvr, "VR_GetGenericInterface");
		if (!VR_GetGenericInterface)
			return false;

		int eError = 0;
		state.system = (vr026::IVRSystem*)VR_GetGenericInterface("IVRSystem_026", &eError);
		if (!state.system || eError != 0) {
			state.system = nullptr;
			return false;
		}
	}

	vr026::HmdVector2_t left{};
	vr026::HmdVector2_t right{};
	if (!state.system->GetEyeTrackedFoveationCenter(&left, &right))
		return false;

	// Diagnostic: log the first successful read and any all-zero reads. A
	// driver that "succeeds" but returns (0,0) every frame is a mock-source
	// trigger in disguise — the caller would fall through to the Lissajous
	// fallback and the overlay would trace a synthetic path.
	static bool loggedFirst = false;
	if (!loggedFirst) {
		loggedFirst = true;
		logger::info("[EYETRACK] OpenVR foveation center read OK: left=({:.3f},{:.3f}) right=({:.3f},{:.3f})",
			left.v[0], left.v[1], right.v[0], right.v[1]);
	}
	static uint32_t zeroReads = 0;
	if (left.v[0] == 0.0f && left.v[1] == 0.0f && right.v[0] == 0.0f && right.v[1] == 0.0f) {
		if (++zeroReads == 100 || zeroReads == 1000) {
			logger::warn("[EYETRACK] OpenVR foveation center returned all-zero {} times — tracker may be inactive", zeroReads);
		}
	}

	state.lastLeft[0] = left.v[0];
	state.lastLeft[1] = left.v[1];
	state.lastRight[0] = right.v[0];
	state.lastRight[1] = right.v[1];
	state.everSucceeded = true;
	state.framesSinceSuccess = 0;
	state.lastSource = GazeSource::kOpenVR;

	outLeft[0] = left.v[0];
	outLeft[1] = left.v[1];
	outRight[0] = right.v[0];
	outRight[1] = right.v[1];
	return true;
}

	/**
	 * @brief Acquire the current eye tracking sample.
	 *
	 * Source selection is sticky: once the real OpenVR tracker has produced data,
	 * a failed frame holds the last valid gaze for a grace period (confidence
	 * lowered) rather than switching sources — alternating with the mock made the
	 * overlay jump between the true gaze and the Lissajous path. After the grace
	 * expires the sample reports invalid (foveation holds center); the mock is
	 * only used while no real tracker has ever been seen.
	 */
	inline EyeTrackingDataVR TryGetEyeTrackingData()
	{
		EyeTrackingDataVR result;
		auto& state = GetGazeSourceState();

		// Real source: per-eye NDC foveation centers from SteamVR.
		if (TryGetOpenVREyeTracking(result.gazeNDCLeft, result.gazeNDCRight)) {
			result.isValid = true;
			result.confidence = 1.0f;
			return result;
		}

		// Real tracker seen before but this frame failed: hold last valid gaze.
		if (state.everSucceeded) {
			constexpr uint32_t kHoldFrames = 45;  // ~0.5s at 90fps
			if (state.framesSinceSuccess < kHoldFrames) {
				state.framesSinceSuccess++;
				result.isValid = true;
				result.confidence = 0.5f;
				result.gazeNDCLeft[0] = state.lastLeft[0];
				result.gazeNDCLeft[1] = state.lastLeft[1];
				result.gazeNDCRight[0] = state.lastRight[0];
				result.gazeNDCRight[1] = state.lastRight[1];
				return result;
			}
			// Grace expired: no data this frame; foveation holds center.
			state.lastSource = GazeSource::kNone;
			return result;
		}

		// Mock fallback: smooth Lissajous figure inside the eye's central region.
		// Development only — never reached once real hardware has been detected.
		using clock = std::chrono::steady_clock;
		const auto now = clock::now().time_since_epoch();
		const float t = std::chrono::duration<float>(now).count();

		constexpr float kAmplitude = 0.35f;
		constexpr float kPeriodX = 7.0f;
		constexpr float kPeriodY = 11.0f;

		const float x = kAmplitude * std::sin(2.0f * 3.14159265f * t / kPeriodX);
		const float y = kAmplitude * std::sin(2.0f * 3.14159265f * t / kPeriodY);

		result.isValid = true;
		result.confidence = 1.0f;
		result.gazeNDCLeft[0] = x;
		result.gazeNDCLeft[1] = y;
		result.gazeNDCRight[0] = x;
		result.gazeNDCRight[1] = y;
		state.lastSource = GazeSource::kMock;
		return result;
	}

	/** @brief Gaze source that produced the last sample (for UI status display). */
	inline GazeSource GetLastGazeSource()
	{
		return GetGazeSourceState().lastSource;
	}
}  // namespace FoveatedRenderEyeTracking
