#pragma once

// ============================================================================
// FoveatedRender — VR DLSS enhancement mode of Upscaling
// ============================================================================
//
// Foveated subrect-DLSS path: only the user-selected region gets full DLSS
// upscaling; the periphery is cheaply stretched via SubrectStretchCS. Halves
// (or more) the DLSS workload. Composes with VRS, Screenshot, and the lossless
// recording feature through the shared Util::Subrect module — use the same
// preset for consistent results across them.
//
// Architecturally a mode inside Upscaling (mirroring DLSSperf): a static-
// inline member, not a peer Feature. Settings that overlap with Upscaling's
// (quality mode, sharpness, DLSS preset, Streamline log level) read directly
// from `globals::features::upscaling.settings` rather than being duplicated.
// VR only -- flat has no lens-driven periphery quality cliff to exploit, so
// the perf/quality tradeoff doesn't carry over. Supports both DLSS and FSR3
// (host path); under FSR, only DlssMode::kDefault is available -- kFaster
// relies on Streamline's SBS-subrect read, which has no FSR equivalent.
//
// ============================================================================

#include "../../Utils/BootSnapshot.h"
#include "../../Utils/Subrect.h"
#include "EyeTrackingFoveation.h"

#include <chrono>
#include <d3d11.h>
#include <winrt/base.h>

struct FoveatedRender
{
	// DLSS execution mode for VR
	enum class DlssMode : uint
	{
		kDefault = 0,  // Per-eye isolation: 2 extra resource sets, 2 evaluates. Supports F/J/K/L/M.
		kFaster = 1,   // SBS viewport: tell SL to read subrect from SBS directly, no extra resources, 2 evaluates. J/K incompatible, only L/M/F.
	};

	// Subrect blend mode when writing DLSS output back over stretched background
	enum class SubrectBlendMode : uint
	{
		kHardCopy = 0,  // CopySubresourceRegion (no blending — sharp edge)
		kFeather = 1,   // smoothstep alpha ramp over N pixels
		kDither = 2,    // Blue-noise binary threshold in feather band
	};

	// Periphery AA algorithm applied after background stretch
	enum class PeripheryAAMode : uint
	{
		kNone = 0,            // No dedicated periphery AA
		kTemporalSmooth = 1,  // Motion-compensated temporal accumulation (anti-flicker)
	};

	// Stretch algorithm for DRS → full-eye background (used by SubrectStretchCS shader)
	enum class StretchMode : uint
	{
		kBilinear = 0,      // Default bilinear sampling (clean upscale)
		kPoint = 1,         // Nearest-neighbor / point (cheapest, VRS-like broadcast)
		kGaussianBlur = 2,  // 3x3 Gaussian blur (soft periphery)
	};

	/** @brief Translated display names for the enums above -- single source shared by
	 *  DrawSettings' dropdowns and Upscaling::GetProfilePreviewText. */
	static const char* DlssModeName(DlssMode mode);
	static const char* StretchModeName(StretchMode mode);
	static const char* PeripheryAAModeName(PeripheryAAMode mode);
	static const char* SubrectBlendModeName(SubrectBlendMode mode);

	// FoveatedRender-specific settings. Quality mode / sharpness / DLSS preset /
	// Streamline log level live on Upscaling::Settings and are read through
	// the accessors below — do not duplicate them here. Sharpening on/off is
	// controlled by the shared sharpnessDLSS slider (0 disables RCAS).
	//
	// Do not add UI sliders for MV-dilation / reactive-mask / transparency-mask
	// toggles until the shader permutations are plumbed: EncodeTexturesCS needs
	// per-toggle defines, the encode pass needs a conditional skip when all are
	// off, and EvaluateDLSS needs per-toggle arg gating. Knobs without wiring
	// are silent no-ops that mislead users.
	struct Settings
	{
		uint enabled = 0;  // opt-in: requires restart to take effect via LatchEnabled()
		uint dlssMode = (uint)DlssMode::kDefault;
		uint stretchMode = (uint)StretchMode::kGaussianBlur;
		float peripheryBlurRadius = 1.0f;
		uint debugVisualize = 0;  // tint cheap-stretched periphery red; runtime toggle
		uint peripheryAAMode = static_cast<uint>(PeripheryAAMode::kTemporalSmooth);
		float peripheryTemporalAlpha = 0.16f;
		uint subrectBlendMode = static_cast<uint>(SubrectBlendMode::kHardCopy);
		float subrectFeatherWidth = 64.0f;
		float subrectDitherStrength = 1.0f;
		bool neuralRenderingEnabled = false;
		uint neuralRenderingPreset = 3;
		float neuralRenderingIntensity = 0.8f;
		float neuralRenderingLocalTone = 0.75f;
		float neuralRenderingLocalStructure = 0.9f;
		float neuralRenderingSkinStructure = 0.9f;
		uint neuralRenderingStyle = 3;
		bool neuralRenderingAutoMask = true;
		bool neuralRenderingUICorrection = false;
		// Eye tracking foveation
		uint eyeTrackingFoveationEnabled = 0;  // Toggle for dynamic gaze-based foveation
		float eyeTrackingGazeSmoothingAlpha = 0.2f;  // Temporal smoothing [0..1]
		float eyeTrackingGazeThresholdPixels = 2.0f;  // Deadzone filter (pixels)
		uint eyeTrackingDebugOverlay = 0;  // 0=off, 1=crosshair, 2=crosshair + vignette mask
		// Per-eye calibration bias (NDC units): subtracted from the raw gaze
		// before smoothing. Compensates a tracker that reads off-center when
		// the user looks straight ahead.
		float eyeTrackingBiasX = 0.0f;   // [-0.5, 0.5]
		float eyeTrackingBiasY = 0.0f;   // [-0.5, 0.5]
	};

	inline static constexpr Util::Settings::RestartTable<Settings, 1> kRestartFields{ {
		UTIL_RESTART_FIELD(Settings, enabled, "Foveated DLSS"),
	} };
	Util::Settings::BootSnapshot<Settings> bootSnapshot{ kRestartFields };

	/** @brief Region-preset display names, shared by PostPostLoad's seed list, the
	 *  top-level preset buttons, and Upscaling::ApplyPerformanceProfile. */
	static constexpr const char* kPresetFullEye = "Full Eye";                          ///< No crop; full-eye DLSS coverage.
	static constexpr const char* kPresetCenter75 = "Center 75%";                       ///< Centered crop covering 75% of the eye.
	static constexpr const char* kPresetCenter50 = "Center 50%";                       ///< Centered crop covering 50% of the eye.
	static constexpr const char* kPresetNasalConvergence50 = "Nasal Convergence 50%";  ///< 50% crop biased toward nasal convergence.

	Settings settings;
	FoveatedRenderEyeTracking::EyeTrackingData eyeTrackingData;  // Eye tracking state and gaze data
	Util::Subrect::Controller subrectController;

	// Last gaze offset applied to the subrect (UV units), used for hysteresis:
	// the subrect only moves when the gaze drifts more than a threshold from
	// this position, so a stationary gaze keeps the subrect (and DLSS temporal
	// history) stable. Tracked per eye — the eyes' offsets move independently
	// under convergence.
	float lastGazeOffsetUV[2] = { 0.0f, 0.0f };
	float lastGazeOffsetRightUV[2] = { 0.0f, 0.0f };

	// True for the frame in which the gaze-following offset moved the subrect.
	// Consumers whose guides were cropped at the previous position (e.g.
	// NeuralRendering) skip that frame to stay aligned with the moved region.
	bool subrectMovedThisFrame = false;

	// Called from Upscaling::DrawSettings. DrawEnable renders the always-visible
	// header + Enable checkbox at the parent's top level; DrawSettings renders
	// the body knobs inside a collapsible TreeNode (Upscaling wraps it in
	// BeginDisabled when settings.enabled == 0).
	void DrawEnable();
	void DrawSettings();
	// Called from Upscaling::SaveSettings / LoadSettings to round-trip JSON.
	void SaveSettings(json& o_json);
	void LoadSettings(const json& o_json);
	void RestoreDefaultSettings();
	void ClearShaderCache();
	// Called from Upscaling::PostPostLoad to seed subrect presets.
	void PostPostLoad();

	struct UICompositeRenderHook
	{
		static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	bool IsRuntimeSupported() const;
	bool IsActive() const;
	bool IsLoaded() const { return enabledAtBoot; }

	/** @brief True while drag-resizing the crop region, and for a few seconds after.
	 *  Read by the stretch pass alongside settings.debugVisualize. */
	bool ShouldForceVisualize() const;

	// Foveation region for per-pixel foveated effects (e.g. SSR): the rectangular DLSS subrect mapped
	// to centered-superellipse params. available is false when foveation is inactive or full-eye.
	struct FoveationProfile
	{
		bool available = false;
		float coverageScale = 1.0f;          // linear center coverage scale [0.25, 1.0]
		float centerHorizontalScale = 1.0f;  // [1.0, 2.0]
		float2 centerOffsets[2] = {};        // [0]=left eye, [1]=right eye
	};
	FoveationProfile GetFoveationProfile() const;

	// Eye tracking foveation: update gaze data and apply dynamic offset
	void UpdateEyeTrackingFoveation();

	// Debug overlay: draw the gaze crosshair (and optional vignette mask) onto
	// the final VR SBS frame. Skeleton-stage validation of the gaze pipeline.
	void DrawGazeDebugOverlay();

	// Main enable: latched at boot, change requires restart
	void LatchEnabled() { enabledAtBoot = (settings.enabled != 0); }

	// Quality mode reads through Upscaling::Settings — latch the boot value so
	// downstream RT allocations stay coherent if the user moves the slider.
	void LatchQualityMode();
	uint GetQualityModeAtBoot() const { return qualityModeAtBoot; }

	/// Render-to-display scale denominator for a quality mode index
	/// (1=Quality .. 4=UltraPerformance). Delegates to the FFX SDK ratio table.
	static float GetRenderScaleForQuality(uint qualityMode);

	// Faster mode relies on Streamline's SBS-subrect read, which has no FSR
	// equivalent -- forced to kDefault whenever FSR is the selected method.
	DlssMode GetDlssMode() const;
	StretchMode GetStretchMode() const { return (StretchMode)std::min(settings.stretchMode, 2u); }
	PeripheryAAMode GetPeripheryAAMode() const { return static_cast<PeripheryAAMode>(std::min(settings.peripheryAAMode, 1u)); }
	SubrectBlendMode GetSubrectBlendMode() const { return static_cast<SubrectBlendMode>(std::min(settings.subrectBlendMode, 2u)); }

	// Active getters: clamp + route shared fields through Upscaling::Settings.
	uint GetActiveQualityMode() const;
	uint GetActivePresetDLSS() const;
	float GetActiveSharpnessDLSS() const;

	// Re-clamp cross-feature settings (preset vs DLSS mode). Idempotent; safe to call
	// from Upscaling::LoadSettings after JSON has overwritten shared fields.
	void ClampSettings();

private:
	bool enabledAtBoot = false;                            // latched from settings.enabled at boot
	uint qualityModeAtBoot = 4;                            // latched from Upscaling::Settings::qualityMode at boot
	std::chrono::steady_clock::time_point lastDragTime{};  // epoch -- no force-visualize at boot

	bool IsPresetCompatibleWithMode(uint presetIndex) const;
	void ClampPresetToMode();
};
