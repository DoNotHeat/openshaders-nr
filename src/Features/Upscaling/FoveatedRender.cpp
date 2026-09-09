#include "FoveatedRender.h"

#include "../../Globals.h"
#include "../../GpuPass.h"
#include "../../I18n/I18n.h"
#include "../../Utils/Game.h"
#include "../../Utils/Subrect.h"
#include "../../Utils/UI.h"
#include "../FoveatedCommon.h"
#include "../Upscaling.h"
#include "EyeTrackingFoveation.h"
#include "FoveatedRender/Core.h"
#include "NeuralRendering/Integration.h"
#include "NeuralRendering/Renderer.h"
#include "../VR/EyeTrackingVR.h"

#include <algorithm>
#include <cmath>

#define I18N_KEY_PREFIX "feature.upscaling."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FoveatedRender::Settings,
	enabled,
	dlssMode,
	stretchMode,
	peripheryBlurRadius,
	debugVisualize,
	peripheryAAMode,
	peripheryTemporalAlpha,
	subrectBlendMode,
	subrectFeatherWidth,
	subrectDitherStrength,
	neuralRenderingEnabled,
	neuralRenderingPreset,
	neuralRenderingIntensity,
	neuralRenderingLocalTone,
	neuralRenderingLocalStructure,
	neuralRenderingSkinStructure,
	neuralRenderingStyle,
	neuralRenderingAutoMask,
	neuralRenderingUICorrection,
	neuralRenderingSkinMaskOnly,
	neuralRenderingCharacterRange,
	neuralRenderingQuality,
	neuralRenderingSkinMaskDebug,
	neuralRenderingDisableWhileSprinting,
	neuralRenderingDisableWhileInCombat,
	eyeTrackingFoveationEnabled,
	eyeTrackingDebugOverlay,
	eyeTrackingBiasX,
	eyeTrackingBiasY,
	eyeTrackingFovDeadzonePx,
	eyeTrackingFovGlideFactor);

// ============================================================================
// Lifecycle
// ============================================================================

void FoveatedRender::PostPostLoad()
{
	bootSnapshot.LatchIfNeeded(settings);

	// Opt into the stereo extension so the controller tracks a separate
	// right-eye UV (HMD nose-side overlap symmetry).
	subrectController.SetStereoEnabled(true);

	// Seed sensible foveal presets. Empty-case only — user edits persist.
	// "Center N%" presets are symmetric per eye (no rightUV → auto-mirror, which
	// for centered UVs produces an identical right-eye UV). "Nasal Convergence"
	// is asymmetric: left eye biased toward its right edge, right eye biased
	// toward its left edge — both targeting the nose-side region where HMD
	// binocular fusion is strongest, so DLSS reconstruction lands in the actual
	// stereo overlap zone rather than diverging left/right fields.
	subrectController.SeedDefaultPresets({
		{ .name = kPresetFullEye, .uv = { 0.0f, 0.0f, 1.0f, 1.0f } },
		{ .name = kPresetCenter75, .uv = { 0.125f, 0.125f, 0.75f, 0.75f } },
		{ .name = kPresetCenter50, .uv = { 0.25f, 0.25f, 0.5f, 0.5f } },
		{ .name = kPresetNasalConvergence50,
			.uv = { 0.5f, 0.25f, 0.5f, 0.5f },
			.rightUV = Util::Subrect::UVRegion{ 0.0f, 0.25f, 0.5f, 0.5f } },
	});
	// PostPostLoad runs after settings load, so a user with an older, shorter
	// persisted preset list (from before these names existed) still sees every
	// current preset in the DrawEditor dropdown, not just whichever ones they
	// happened to click as buttons.
	subrectController.MaterializeNewDefaults();
	stl::write_vfunc<0x1, UICompositeRenderHook>(RE::VTABLE_BSImagespaceShaderCopyDynamicFetchDisabled[3]);
}

void FoveatedRender::UICompositeRenderHook::thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	NeuralRendering::ApplyFoveatedLdr();
	func(imageSpaceShader, shape, param);
}

void FoveatedRender::ClearShaderCache()
{
	FoveatedRenderImpl::Core::ClearShaderCache();
	FoveatedRenderImpl::Core::ClearResources();
}

// ============================================================================
// Settings I/O — driven from Upscaling::Save/LoadSettings under a nested key
// ============================================================================

void FoveatedRender::SaveSettings(json& o_json)
{
	o_json = settings;
	subrectController.SaveSettings(o_json);
}

void FoveatedRender::LoadSettings(const json& o_json)
{
	settings = o_json;
	// Util::Subrect::Controller::LoadSettings takes `const json&` (Subrect.h:68)
	// so no const_cast is needed — keeping it would imply mutation that never
	// happens.
	subrectController.LoadSettings(o_json);
	ClampSettings();
}

void FoveatedRender::RestoreDefaultSettings()
{
	settings = {};
	ClampSettings();
}

void FoveatedRender::ClampSettings()
{
	settings.enabled = std::min(settings.enabled, 1u);
	settings.dlssMode = std::min(settings.dlssMode, 1u);
	settings.stretchMode = std::min(settings.stretchMode, 2u);
	settings.debugVisualize = std::min(settings.debugVisualize, 1u);
	settings.peripheryAAMode = std::min(settings.peripheryAAMode, 1u);
	settings.subrectBlendMode = std::min(settings.subrectBlendMode, 2u);
	settings.peripheryBlurRadius = std::clamp(settings.peripheryBlurRadius, 0.5f, 4.0f);
	settings.peripheryTemporalAlpha = std::clamp(settings.peripheryTemporalAlpha, 0.05f, 0.5f);
	settings.subrectFeatherWidth = std::clamp(settings.subrectFeatherWidth, 2.0f, 128.0f);
	settings.subrectDitherStrength = std::clamp(settings.subrectDitherStrength, 0.0f, 2.0f);
	settings.neuralRenderingPreset = std::min(settings.neuralRenderingPreset, 4u);
	settings.neuralRenderingCharacterRange = std::clamp(settings.neuralRenderingCharacterRange, 0.0f, 16384.0f);
	// Neural quality is a dropdown preset (100/90/75/50/25); snap any stale or
	// hand-edited value to the nearest valid option.
	{
		static constexpr uint kQualityOptions[] = { 100, 90, 75, 50, 25 };
		uint best = kQualityOptions[0];
		uint bestDiff = UINT_MAX;
		for (uint option : kQualityOptions) {
			const uint diff = option > settings.neuralRenderingQuality ? option - settings.neuralRenderingQuality :
			                                                           settings.neuralRenderingQuality - option;
			if (diff < bestDiff) {
				bestDiff = diff;
				best = option;
			}
		}
		settings.neuralRenderingQuality = best;
	}
	settings.neuralRenderingIntensity = std::clamp(settings.neuralRenderingIntensity, 0.0f, 2.0f);
	settings.neuralRenderingLocalTone = std::clamp(settings.neuralRenderingLocalTone, 0.0f, 2.0f);
	settings.neuralRenderingLocalStructure = std::clamp(settings.neuralRenderingLocalStructure, 0.0f, 2.0f);
	settings.neuralRenderingSkinStructure = std::clamp(settings.neuralRenderingSkinStructure, 0.0f, 2.0f);
	settings.neuralRenderingStyle = std::min(settings.neuralRenderingStyle, 3u);
	settings.eyeTrackingFoveationEnabled = std::min(settings.eyeTrackingFoveationEnabled, 1u);
	settings.eyeTrackingDebugOverlay = std::min(settings.eyeTrackingDebugOverlay, 2u);
	settings.eyeTrackingBiasX = std::clamp(settings.eyeTrackingBiasX, -0.5f, 0.5f);
	settings.eyeTrackingBiasY = std::clamp(settings.eyeTrackingBiasY, -0.5f, 0.5f);
	settings.eyeTrackingFovDeadzonePx = std::clamp(settings.eyeTrackingFovDeadzonePx, 50.0f, 800.0f);
	settings.eyeTrackingFovGlideFactor = std::clamp(settings.eyeTrackingFovGlideFactor, 0.05f, 1.0f);
	// Preset clamping reads from Upscaling::Settings now.
	auto& sharedPreset = globals::features::upscaling.settings.presetDLSS;
	sharedPreset = std::min(sharedPreset, 5u);
	if (!IsPresetCompatibleWithMode(sharedPreset)) {
		sharedPreset = 3;  // Fall back to L
	}
}

// ============================================================================
// Activation + accessors
// ============================================================================

bool FoveatedRender::IsActive() const
{
	// Gate on DLSS/FSR being the *selected* method, not just available
	// (IsRuntimeSupported): otherwise foveation and its SSR consumer run under
	// TAA/None and the route derefs unallocated upscaler resources — the crash
	// seen under RenderDoc, whose DX12 swapchain disables DLSS.
	if (!enabledAtBoot || !IsRuntimeSupported())
		return false;
	const auto method = globals::features::upscaling.GetUpscaleMethod();
	if (method != Upscaling::UpscaleMethod::kDLSS && method != Upscaling::UpscaleMethod::kFSR)
		return false;

	// A Full Eye region pays the isolation/stretch overhead (snapshot copies, mask
	// clears, StretchDRS) for a subrect equal to the full frame -- no savings, real
	// cost. Skip the route so the standard full-frame path runs instead.
	const bool isFullEye = subrectController.GetUV().IsFullEye() && subrectController.GetRightEyeUV().IsFullEye();
	return !isFullEye;
}

bool FoveatedRender::ShouldForceVisualize() const
{
	using namespace std::chrono_literals;
	return std::chrono::steady_clock::now() - lastDragTime < 3s;
}

bool FoveatedRender::IsRuntimeSupported() const
{
	// FSR's host path has no adapter/runtime prerequisite, so VR alone is
	// sufficient to enable the option -- IsActive() is what actually gates
	// on the *selected* method (kDLSS/kFSR) being one the route supports.
	return globals::game::isVR;
}

FoveatedRender::DlssMode FoveatedRender::GetDlssMode() const
{
	if (globals::features::upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kFSR)
		return DlssMode::kDefault;
	return (DlssMode)std::min(settings.dlssMode, 1u);
}

FoveatedRender::FoveationProfile FoveatedRender::GetFoveationProfile() const
{
	FoveationProfile profile;
	if (!IsActive())
		return profile;

	const auto& leftUV = subrectController.GetUV();
	const auto& rightUV = subrectController.GetRightEyeUV();

	// Map the rectangular subrect onto the centered superellipse the mask helper expects: vertical
	// extent drives coverageScale (radiusY = coverageScale/2), the rect aspect drives the horizontal
	// stretch (radiusX = coverageScale * hScale/2). The mask carries one scale for both eyes (only
	// the center offset is per-eye), so size comes from the less-foveated (larger) extent of the two:
	// the center is the superset enclosing both eyes' full-quality regions, so neither eye's sharp
	// zone is ever foveated (min would shrink it below an eye's sharp region and foveate it). A
	// full eye therefore yields full coverage and disables foveation (the gate below).
	const float coverageH = std::max(leftUV.h, rightUV.h);
	const float coverageW = std::max(leftUV.w, rightUV.w);
	const float coverageScale = FoveatedCommon::ClampCenterScale(coverageH);

	// Availability keys off the clamped scale: if the larger eye rounds up to full coverage there is
	// nothing to foveate, leave the default (available == false).
	if (!FoveatedCommon::IsActiveCoverage(coverageScale))
		return profile;

	profile.available = true;
	profile.coverageScale = coverageScale;
	profile.centerHorizontalScale = FoveatedCommon::ClampCenterHorizontalScale(
		coverageH > 1e-4f ? coverageW / coverageH : 1.0f);
	profile.centerOffsets[0] = { (leftUV.x + leftUV.w * 0.5f) - 0.5f, (leftUV.y + leftUV.h * 0.5f) - 0.5f };
	profile.centerOffsets[1] = { (rightUV.x + rightUV.w * 0.5f) - 0.5f, (rightUV.y + rightUV.h * 0.5f) - 0.5f };
	return profile;
}

void FoveatedRender::UpdateEyeTrackingFoveation()
{
	// Skeleton stage: acquire gaze (mock source until OpenVR wiring lands),
	// smooth it, publish the smoothed point for the debug overlay, and shift
	// the high-quality subrect to follow the gaze.
	if (!globals::game::isVR || !settings.eyeTrackingFoveationEnabled) {
		subrectMovedThisFrame = false;
		return;
	}
	static bool logOnce = false;
	if (!logOnce) {
		logger::info("[EYETRACK] UpdateEyeTrackingFoveation running (isVR={} enabled={})", globals::game::isVR, settings.eyeTrackingFoveationEnabled);
		logOnce = true;
	}

	auto sample = FoveatedRenderEyeTracking::TryGetEyeTrackingData();
	if (!sample.isValid) {
		eyeTrackingData.isValid = false;
		return;
	}

	// Per-eye temporal smoothing (exponential moving average). Applied every
	// frame unconditionally: a deadzone gate here makes the smoothed point
	// freeze on sub-threshold jitter then jump when the threshold trips,
	// which reads as erratic "chaotic" motion on a running mock source.
	// Constant EMA keeps the overlay tracing the gaze smoothly. The alpha is
	// fixed (not user-tunable): the subrect's movement responsiveness is
	// governed by FOV Move Threshold + Glide Factor below, and a second
	// smoothing knob on the same motion read as a duplicate.
	//
	// The EMA tail is also what caused double redraws: after a saccade the
	// smoothed target keeps converging toward the raw gaze for ~10 frames,
	// so a region that jumped to the smoothed point was still ~300 px behind
	// the real gaze and a second gate trip re-jumped it. A two-stage filter
	// fixes this without a visible glide: the RAW point is used for the
	// subrect target (no tail to chase), while the EMA point stays for the
	// debug overlay. Deadzone/dwell still gate the jump, so tracker noise
	// cannot teleport the region.
	constexpr float kGazeSmoothingAlpha = 0.2f;
	// Per-eye calibration bias: subtract before smoothing so the whole
	// pipeline (overlay, subrect offset) sees the corrected point.
	const float bias[2] = { settings.eyeTrackingBiasX, settings.eyeTrackingBiasY };
	for (uint eye = 0; eye < 2; ++eye) {
		const float* raw = (eye == 0) ? sample.gazeNDCLeft : sample.gazeNDCRight;
		float* smoothed = (eye == 0) ? eyeTrackingData.smoothedGazeLeft : eyeTrackingData.smoothedGazeRight;
		float* published = (eye == 0) ? eyeTrackingData.gazeNDCLeft : eyeTrackingData.gazeNDCRight;

		const float corrected[2] = { raw[0] - bias[0], raw[1] - bias[1] };
		FoveatedRenderEyeTracking::SmoothGazePoint(corrected, smoothed, kGazeSmoothingAlpha, smoothed);
		published[0] = smoothed[0];
		published[1] = smoothed[1];
	}

	// Move the high-quality subrect to follow the gaze. The offset is in UV
	// units of the per-eye region: GazeToSubrectOffset maps the clamped NDC
	// gaze (-0.4..0.4) onto a max offset, so passing a UV-space max keeps the
	// result in UV units. Gaze NDC y is up while UV y is down, so the Y offset
	// is negated. SetGazeOffset applies the offset on top of the base crop
	// (never accumulating) and mirrors the X offset for the right eye.
	// The subrect target uses the RAW corrected gaze, not the EMA point: a
	// single-jump region must land where the eye actually is, or the EMA
	// convergence tail forces a second gate trip (double redraw).
	const float maxSubrectOffsetUV = 0.3f;
	float rawL[2];
	float rawR[2];
	{
		const float rawLeftNDC[2] = {
			sample.gazeNDCLeft[0] - settings.eyeTrackingBiasX,
			sample.gazeNDCLeft[1] - settings.eyeTrackingBiasY
		};
		const float rawRightNDC[2] = {
			sample.gazeNDCRight[0] - settings.eyeTrackingBiasX,
			sample.gazeNDCRight[1] - settings.eyeTrackingBiasY
		};
		FoveatedRenderEyeTracking::GazeToSubrectOffset(rawLeftNDC, maxSubrectOffsetUV, rawL);
		FoveatedRenderEyeTracking::GazeToSubrectOffset(rawRightNDC, maxSubrectOffsetUV, rawR);
	}
	float offsetL[2];
	float offsetR[2];
	offsetL[0] = rawL[0];
	offsetL[1] = rawL[1];
	offsetR[0] = rawR[0];
	offsetR[1] = rawR[1];

	const auto renderSize = Util::ConvertToDynamic(globals::state->screenSize);
	const float eyeWidthPx = std::max(1.0f, renderSize.x * 0.5f);
	const float eyeHeightPx = std::max(1.0f, renderSize.y);

	// Movement threshold in pixels — user-tunable. Must be comfortably above
	// tracker noise so a still gaze never trips it; large enough that the
	// subrect moves only on deliberate gaze shifts. Evaluated per eye: under
	// convergence the eyes' offsets move independently, and either eye's move
	// must update its square.
	//
	// The raw threshold alone cannot hold the region still: human gaze is
	// never stationary (fixation drift, microsaccades) and the tracker adds
	// its own wander, so the drift crosses any small threshold every few
	// seconds. Two stability layers fix that:
	// 1. Deadzone: the region re-centers only when the gaze has drifted the
	//    configured deadzone AWAY FROM THE ANCHORED POINT. Natural fixation
	//    wander stays well inside it, so the region — and the DLSS/NR
	//    temporal accumulation — holds still through normal looking.
	// 2. Dwell: the drift must exceed the deadzone for kGazeDwellFrames
	//    consecutive frames before the glide starts, so transient spikes
	//    (convergence jumps, tracker glitches) never move the region.
	const float deadzonePx = settings.eyeTrackingFovDeadzonePx;
	constexpr uint kGazeDwellFrames = 8;
	// Saccade bypass: fixation drift moves slower than ~15 px/frame, a real
	// gaze shift covers hundreds. A drift this fast is deliberate — start the
	// glide immediately instead of waiting out the dwell gate, which delays
	// the region until after the eye has already focused on the new target.
	constexpr float kSaccadeSpeedPx = 40.0f;
	// Persists across frames: saccade speed is the per-frame growth of the
	// drift distance, so it needs last frame's drift per eye.
	static float prevDriftPx[2] = { 0.0f, 0.0f };
	auto gazeMovedPx = [&](const float* off, const float* last) {
		const float dxPx = (off[0] - last[0]) * eyeWidthPx;
		const float dyPx = (off[1] - last[1]) * eyeHeightPx;
		return std::sqrt(dxPx * dxPx + dyPx * dyPx);
	};
	const float leftDriftPx = gazeMovedPx(offsetL, lastGazeOffsetUV);
	const float rightDriftPx = gazeMovedPx(offsetR, lastGazeOffsetRightUV);
	const bool leftOver = leftDriftPx >= deadzonePx;
	const bool rightOver = rightDriftPx >= deadzonePx;
	gazeOverThresholdFrames[0] = leftOver ? gazeOverThresholdFrames[0] + 1 : 0;
	gazeOverThresholdFrames[1] = rightOver ? gazeOverThresholdFrames[1] + 1 : 0;
	const bool leftSaccade = leftOver && leftDriftPx - prevDriftPx[0] >= kSaccadeSpeedPx;
	const bool rightSaccade = rightOver && rightDriftPx - prevDriftPx[1] >= kSaccadeSpeedPx;
	prevDriftPx[0] = leftDriftPx;
	prevDriftPx[1] = rightDriftPx;
	const bool leftMoved = gazeOverThresholdFrames[0] >= kGazeDwellFrames || leftSaccade;
	const bool rightMoved = gazeOverThresholdFrames[1] >= kGazeDwellFrames || rightSaccade;
	// Binocular lock: the two zones must glide in the same frames. The eyes'
	// gates trip a frame or two apart under convergence/tracker noise, and
	// staggered zone moves read as binocular flicker. Either eye tripping
	// starts BOTH glides this frame — each toward its own gaze target.
	// Glide latch: the EMA gaze target keeps drifting for several frames
	// after a saccade, so the drift can fall back under the 2x re-arm
	// deadzone mid-move and the glide stops — then re-triggers on the next
	// dwell, redrawing the region twice. Latch the glide ON at trip time and
	// keep moving both zones until they reach their targets.
	if (leftMoved || rightMoved)
		gazeGlideActive = true;
	const bool anyMoved = gazeGlideActive;
	subrectMovedThisFrame = anyMoved;
	if (subrectMovedThisFrame) {
		// Jump/glide toward the target. The target is the RAW corrected gaze,
		// so a 1.0 glide factor (instant jump) lands exactly where the eye is
		// and no tail follows — the EMA point would force a second gate trip.
		// Lower factors still glide for users who prefer motion over jumps.
		// Pinhole reprojection lets DLSS follow any step size without losing
		// its history, and NR fades via its own intensity easing.
		const float glide = settings.eyeTrackingFovGlideFactor;
		auto glideToward = [&](float* last, const float* target) {
			last[0] += (target[0] - last[0]) * glide;
			last[1] += (target[1] - last[1]) * glide;
		};
		const float prevL[2] = { lastGazeOffsetUV[0], lastGazeOffsetUV[1] };
		const float prevR[2] = { lastGazeOffsetRightUV[0], lastGazeOffsetRightUV[1] };
		if (anyMoved)
			glideToward(lastGazeOffsetUV, offsetL);
		if (anyMoved)
			glideToward(lastGazeOffsetRightUV, offsetR);
		subrectController.SetGazeOffset(lastGazeOffsetUV[0], -lastGazeOffsetUV[1],
			lastGazeOffsetRightUV[0], -lastGazeOffsetRightUV[1]);
		// Report the move only while the glide step is meaningful: NR pauses on
		// this flag, and holding the pause through the glide's tail (sub-pixel
		// steps) would keep NR off for most of a second per recenter.
		const float stepPx = std::max(
			gazeMovedPx(lastGazeOffsetUV, prevL), gazeMovedPx(lastGazeOffsetRightUV, prevR));
		subrectMovedThisFrame = stepPx > 2.0f;
		// End the glide once both zones have essentially arrived. With the raw
		// target this trips on the very next frame after a 1.0 jump; it only
		// matters for sub-1.0 glide factors.
		const float remainingPx = std::max(
			gazeMovedPx(offsetL, lastGazeOffsetUV), gazeMovedPx(offsetR, lastGazeOffsetRightUV));
		if (remainingPx < 16.0f)
			gazeGlideActive = false;
		// Per-frame glide diagnostics: one line per eye per moving frame.
		// step= is this frame's applied step, remain= distance still open to
		// the (drifting) EMA target, gate= what triggered this frame's move
		// (L latch | S saccade | D dwell), lat= latch state after this frame.
		// Reveals whether a perceived double redraw is one continuous glide
		// or two separate gate trips (two L-runs in the log).
		static uint glideLogBudget = 3000;
		if (glideLogBudget > 0) {
			--glideLogBudget;
			logger::info("[GLIDE-DIAG] frame={} eye=L step={:.1f} remain={:.1f} drift={:.1f} gate={}{}{} latch={}",
				globals::state ? globals::state->frameCount : 0, gazeMovedPx(lastGazeOffsetUV, prevL),
				gazeMovedPx(offsetL, lastGazeOffsetUV), leftDriftPx,
				gazeOverThresholdFrames[0] >= kGazeDwellFrames ? 'D' : '-',
				leftSaccade ? 'S' : '-', gazeGlideActive ? 'L' : '-', gazeGlideActive ? 1 : 0);
			logger::info("[GLIDE-DIAG] frame={} eye=R step={:.1f} remain={:.1f} drift={:.1f} gate={}{}{} latch={}",
				globals::state ? globals::state->frameCount : 0, gazeMovedPx(lastGazeOffsetRightUV, prevR),
				gazeMovedPx(offsetR, lastGazeOffsetRightUV), rightDriftPx,
				gazeOverThresholdFrames[1] >= kGazeDwellFrames ? 'D' : '-',
				rightSaccade ? 'S' : '-', gazeGlideActive ? 'L' : '-', gazeGlideActive ? 1 : 0);
		}
		// Re-arm the dwell counters against the NEW position with a 2x
		// deadzone: the region must not re-trigger until the gaze has drifted
		// well past the just-applied offset.
		gazeOverThresholdFrames[0] = (leftDriftPx >= 2.0f * deadzonePx) ? kGazeDwellFrames : 0;
		gazeOverThresholdFrames[1] = (rightDriftPx >= 2.0f * deadzonePx) ? kGazeDwellFrames : 0;
	}

	eyeTrackingData.isValid = true;
}

// The gaze debug overlay is drawn inside the foveated stretch pass
	// (SubrectStretchCS.hlsl), i.e. the same compute shader that renders the
	// red "Visualize regions" tint — the proven path to the final frame for
	// this foveation route. This dedicated method is intentionally unused; it
	// would paint kVR_FRAMEBUFFER too late (kMAIN is already folded into it).
void FoveatedRender::DrawGazeDebugOverlay()
{
}

void FoveatedRender::LatchQualityMode()
{
	qualityModeAtBoot = std::clamp(globals::features::upscaling.settings.qualityMode, 1u, 4u);
}

uint FoveatedRender::GetActiveQualityMode() const
{
	return std::clamp(globals::features::upscaling.settings.qualityMode, 1u, 4u);
}

uint FoveatedRender::GetActivePresetDLSS() const
{
	return std::min(globals::features::upscaling.settings.presetDLSS, 5u);
}

float FoveatedRender::GetActiveSharpnessDLSS() const
{
	return std::clamp(globals::features::upscaling.settings.sharpnessDLSS, 0.0f, 1.0f);
}

float FoveatedRender::GetRenderScaleForQuality(uint qualityMode)
{
	return Upscaling::GetQualityModeRatio(qualityMode);
}

bool FoveatedRender::IsPresetCompatibleWithMode(uint presetIndex) const
{
	// Preset indices: 0=Default, 1=J, 2=K, 3=L, 4=M, 5=F
	// Faster mode: J(1) and K(2) are incompatible.
	if (GetDlssMode() == DlssMode::kFaster) {
		return presetIndex != 1 && presetIndex != 2;
	}
	return true;
}

void FoveatedRender::ClampPresetToMode()
{
	auto& sharedPreset = globals::features::upscaling.settings.presetDLSS;
	if (!IsPresetCompatibleWithMode(sharedPreset)) {
		sharedPreset = 3;  // Fall back to L
	}
}

// ============================================================================
// UI — FoveatedRender-specific knobs only. Quality / sharpness / preset /
// Streamline log level live on Upscaling's panel and apply to both DLSS paths.
// Called from Upscaling::DrawSettings inside a TreeNode.
// ============================================================================

void FoveatedRender::DrawEnable()
{
	ClampSettings();

	ImGui::TextWrapped(T(TKEY("foveated_overview"),
		"Foveated subrect upscaling: only the user-selected region gets full DLSS/FSR "
		"upscaling, the periphery is cheaply stretched. Significant upscaler cost reduction "
		"at the cost of peripheral sharpness. VR only."));

	const bool runtimeSupported = IsRuntimeSupported();
	if (!runtimeSupported) {
		settings.enabled = 0;
	}

	if (!runtimeSupported)
		ImGui::BeginDisabled();
	bool enabledBool = settings.enabled != 0;
	if (ImGui::Checkbox(T(TKEY("foveated_enable"), "Enable Foveated Upscaling (region source)"), &enabledBool)) {
		settings.enabled = enabledBool ? 1u : 0u;
		// Full Eye is a deliberate no-op (see IsActive()) -- enabling straight from
		// it would silently do nothing until the user finds the region picker below.
		if (enabledBool && subrectController.GetUV().IsFullEye() && subrectController.GetRightEyeUV().IsFullEye())
			subrectController.ApplyPresetByName(kPresetCenter75);
	}
	if (!runtimeSupported)
		ImGui::EndDisabled();

	Util::UI::DrawSettingDiff(bootSnapshot, settings, &Settings::enabled);

	if (enabledAtBoot) {
		const auto method = globals::features::upscaling.GetUpscaleMethod();
		const bool methodOk = method == Upscaling::UpscaleMethod::kDLSS || method == Upscaling::UpscaleMethod::kFSR;
		if (IsActive())
			Util::Text::WrappedInfo(T(TKEY("foveated_active"), "Active: foveated subrect upscaling is enabled (skipped in menus / on preflight failure)."));
		else if (!methodOk)
			Util::Text::Warning(T(TKEY("foveated_standing_by"), "Standing by: only active while the Upscaling Method is DLSS or FSR. Inactive right now."));
		else
			Util::Text::Warning(T(TKEY("foveated_standing_by_full_eye"), "Standing by: region is Full Eye (no crop) -- shrink it in Subrect Region below to see savings."));
	}

	if (!globals::game::isVR) {
		Util::Text::Warning(T(TKEY("foveated_vr_only"), "VR only -- flat has no equivalent lens-driven periphery quality cliff to exploit."));
	}
}

const char* FoveatedRender::DlssModeName(DlssMode mode)
{
	return mode == DlssMode::kFaster ?
	           T(TKEY("foveated_dlss_mode_faster"), "Faster") :
	           T(TKEY("foveated_dlss_mode_default"), "Default");
}

const char* FoveatedRender::StretchModeName(StretchMode mode)
{
	switch (mode) {
	case StretchMode::kPoint:
		return T(TKEY("foveated_stretch_point"), "Point");
	case StretchMode::kGaussianBlur:
		return T(TKEY("foveated_stretch_gaussian"), "Gaussian Blur");
	default:
		return T(TKEY("foveated_stretch_bilinear"), "Bilinear");
	}
}

const char* FoveatedRender::PeripheryAAModeName(PeripheryAAMode mode)
{
	return mode == PeripheryAAMode::kTemporalSmooth ?
	           T(TKEY("foveated_periphery_aa_temporal"), "Temporal Smooth") :
	           T(TKEY("foveated_periphery_aa_none"), "None");
}

const char* FoveatedRender::SubrectBlendModeName(SubrectBlendMode mode)
{
	switch (mode) {
	case SubrectBlendMode::kFeather:
		return T(TKEY("foveated_blend_feather"), "Feather");
	case SubrectBlendMode::kDither:
		return T(TKEY("foveated_blend_dither"), "Dither");
	default:
		return T(TKEY("foveated_blend_hard_copy"), "Hard Copy");
	}
}

void FoveatedRender::DrawSettings()
{
	ClampSettings();

	if (globals::game::isVR)
		Util::Text::WrappedInfo(T(TKEY("foveated_shared_panel_note"), "Quality and Sharpness are on the main Upscaling panel — changes there apply to foveated rendering too. DLSS Preset also applies there when DLSS is the selected upscaler."));

	if (ImGui::CollapsingHeader(T(TKEY("neural_rendering_header"), "DLSS Neural Rendering"), ImGuiTreeNodeFlags_DefaultOpen)) {
		const bool supportedRoute = globals::features::upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS &&
			!globals::features::upscaling.IsFrameGenerationConfiguredForSession() &&
			(!globals::game::isVR || (GetDlssMode() == DlssMode::kDefault &&
				globals::features::upscaling.perfMode.IsHookActive()));
		if (!supportedRoute) {
			if (globals::features::upscaling.IsFrameGenerationConfiguredForSession())
				Util::Text::Warning("Disable Frame Generation and restart the game before enabling DLSS Neural Rendering.");
			else
				Util::Text::Warning(T(TKEY("neural_rendering_unavailable"),
					"Requires DLSS. VR additionally requires Foveated Default mode and active PerfMode."));
			ImGui::BeginDisabled();
		}
		ImGui::Checkbox(T(TKEY("neural_rendering_enable"), "Enable DLSS Neural Rendering"), &settings.neuralRenderingEnabled);

		if (settings.neuralRenderingEnabled) {
			static const char* presets[] = { "Custom", "Balanced", "Fabric Detail", "Natural", "Strong" };
			int preset = static_cast<int>(settings.neuralRenderingPreset);
			if (ImGui::Combo(T(TKEY("neural_rendering_preset"), "Tuning Preset"), &preset, presets, IM_ARRAYSIZE(presets))) {
				settings.neuralRenderingPreset = static_cast<uint>(preset);
				switch (settings.neuralRenderingPreset) {
				case 1: settings.neuralRenderingIntensity = 1.0f; settings.neuralRenderingLocalTone = 1.0f; settings.neuralRenderingLocalStructure = 1.0f; settings.neuralRenderingSkinStructure = 1.0f; break;
				case 2: settings.neuralRenderingIntensity = 1.35f; settings.neuralRenderingLocalTone = 0.9f; settings.neuralRenderingLocalStructure = 1.6f; settings.neuralRenderingSkinStructure = 1.15f; break;
				case 3: settings.neuralRenderingIntensity = 0.8f; settings.neuralRenderingLocalTone = 0.75f; settings.neuralRenderingLocalStructure = 0.9f; settings.neuralRenderingSkinStructure = 0.9f; break;
				case 4: settings.neuralRenderingIntensity = 1.75f; settings.neuralRenderingLocalTone = 1.25f; settings.neuralRenderingLocalStructure = 1.5f; settings.neuralRenderingSkinStructure = 1.3f; break;
				default: break;
				}
			}
			bool custom = false;
			custom |= ImGui::SliderFloat(T(TKEY("neural_rendering_intensity"), "Intensity"), &settings.neuralRenderingIntensity, 0.0f, 2.0f, "%.2f");
			custom |= ImGui::SliderFloat(T(TKEY("neural_rendering_local_tone"), "Local Tone"), &settings.neuralRenderingLocalTone, 0.0f, 2.0f, "%.2f");
			custom |= ImGui::SliderFloat(T(TKEY("neural_rendering_local_structure"), "Local Structure"), &settings.neuralRenderingLocalStructure, 0.0f, 2.0f, "%.2f");
			custom |= ImGui::SliderFloat(T(TKEY("neural_rendering_skin_structure"), "Skin Structure"), &settings.neuralRenderingSkinStructure, 0.0f, 2.0f, "%.2f");
			static const char* styles[] = { "Style 0", "Style 1", "Style 2", "Style 3" };
			int style = static_cast<int>(settings.neuralRenderingStyle);
			if (ImGui::Combo(T(TKEY("neural_rendering_style"), "Style"), &style, styles, IM_ARRAYSIZE(styles))) {
				settings.neuralRenderingStyle = static_cast<uint>(style);
				custom = true;
			}
			custom |= ImGui::Checkbox(T(TKEY("neural_rendering_auto_mask"), "Automatic Mask"), &settings.neuralRenderingAutoMask);
			custom |= ImGui::Checkbox(T(TKEY("neural_rendering_ui_correction"), "UI Correction"), &settings.neuralRenderingUICorrection);
			custom |= ImGui::Checkbox(T(TKEY("neural_rendering_skin_mask_only"), "Characters Only"),
				&settings.neuralRenderingSkinMaskOnly);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("neural_rendering_skin_mask_only_tooltip"),
					"Limits the neural rendering effect to characters (face, body, hair, armour) using the character "
					"mask written by the lighting pass. The rest of the frame keeps the original pixels. When no "
					"character is on screen the whole neural pass is skipped, saving its GPU cost. Characters closer "
					"than the Range setting additionally enable the effect; beyond it the pass is skipped."));
			}
			if (settings.neuralRenderingSkinMaskOnly) {
				custom |= ImGui::SliderFloat(T(TKEY("neural_rendering_character_range"), "Character Range"),
					&settings.neuralRenderingCharacterRange, 0.0f, 8192.0f, "%.0f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("neural_rendering_character_range_tooltip"),
						"Distance to the nearest character beyond which the neural pass is skipped entirely. "
						"0 disables the distance gating (always evaluate when a character is visible)."));
				}
			}
			{
				// Quality dropdown: discrete render-scale presets. Cost scales with the
				// evaluated area — 50% quality costs ~25% of the neural pass.
				static const char* qualityOptions[] = { "100% (Full)", "90%", "75%", "50%", "25%" };
				static const uint qualityValues[] = { 100, 90, 75, 50, 25 };
				int qualityIndex = 0;
				for (int i = 0; i < 5; ++i)
					if (settings.neuralRenderingQuality == qualityValues[i])
						qualityIndex = i;
				if (ImGui::Combo(T(TKEY("neural_rendering_quality"), "Neural Quality"), &qualityIndex, qualityOptions, IM_ARRAYSIZE(qualityOptions))) {
					settings.neuralRenderingQuality = qualityValues[qualityIndex];
					custom = true;
				}
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("neural_rendering_quality_tooltip"),
						"Internal resolution of the neural pass as a percentage of the foveal region: the network "
						"evaluates the whole region at reduced scale and the result is stretched back. Lower = fewer "
						"pixels for the network = higher FPS at reduced neural detail. VR foveated route only."));
				}
			}
			if (settings.neuralRenderingSkinMaskOnly) {
				custom |= ImGui::Checkbox(T(TKEY("neural_rendering_skin_mask_debug"), "Debug: Visualize Character Mask"),
					&settings.neuralRenderingSkinMaskDebug);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("neural_rendering_skin_mask_debug_tooltip"),
						"Renders the character mask instead of the composite: green = character pixels receiving the "
						"neural effect, black = untouched pixels. Use this to verify mask coverage before judging the blend."));
				}
			}
			ImGui::Separator();
			ImGui::TextUnformatted(T(TKEY("neural_rendering_gating_header"), "Gameplay Gating"));
			custom |= ImGui::Checkbox(T(TKEY("neural_rendering_disable_sprinting"), "Disable While Sprinting"),
				&settings.neuralRenderingDisableWhileSprinting);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("neural_rendering_disable_sprinting_tooltip"),
					"Skips the whole neural pass while the player sprints — the effect is barely visible at sprint "
					"speed and the pass costs real GPU time exactly when frames matter most. Re-enables shortly "
					"after sprinting stops."));
			}
			custom |= ImGui::Checkbox(T(TKEY("neural_rendering_disable_combat"), "Disable While In Combat"),
				&settings.neuralRenderingDisableWhileInCombat);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("neural_rendering_disable_combat_tooltip"),
					"Skips the whole neural pass while the player is in combat. Combat is when the frames matter "
					"most and the effect is least noticed."));
			}
			if (custom)
				settings.neuralRenderingPreset = 0;

			auto& neuralRenderer = NeuralRendering::Renderer::Instance();
			if (neuralRenderer.IsFailureLatched()) {
				Util::Text::Warning("DLSS Neural Rendering failed and is disabled for this session. Check CommunityShaders.log.");
				if (ImGui::Button("Reset Neural Rendering Failure"))
					neuralRenderer.Reset();
			}
			if (globals::state && globals::state->IsDeveloperMode()) {
				ImGui::TextDisabled("Status: %s | NGX: 0x%08X | Evaluations: %llu",
					neuralRenderer.StatusText(), neuralRenderer.NgxResult(),
					static_cast<unsigned long long>(neuralRenderer.SuccessfulFrames()));
			}
		}
		if (!supportedRoute)
			ImGui::EndDisabled();
	}

	// ── VR-only knobs ──
	if (globals::game::isVR) {
		ImGui::Separator();
		ImGui::Text("%s", T(TKEY("foveated_dlss_mode_header"), "VR DLSS Mode"));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("foveated_dlss_mode_tooltip"),
								  "Default — highest quality. Each eye gets its own isolated copy of color/depth/motion\n"
								  "vectors so DLSS can't sample across the stereo midline. 5 copies per eye per frame.\n"
								  "All DLSS presets supported. Best for screenshots or when Faster shows edge artifacts.\n"
								  "\n"
								  "Faster — lower overhead. DLSS reads directly from the frame buffer using a viewport\n"
								  "offset instead of isolating each eye. 1 snapshot + 2 mask clears per frame.\n"
								  "DLSS may sample 1-2 pixels from the neighboring eye near the stereo center — usually\n"
								  "invisible in motion. Presets J and K are incompatible and auto-clamp to L."));
		}

		const bool isFSR = globals::features::upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kFSR;
		if (isFSR)
			ImGui::BeginDisabled();
		uint prevMode = settings.dlssMode;
		ImGui::SliderInt(T(TKEY("foveated_dlss_mode_label"), "DLSS Mode"), reinterpret_cast<int*>(&settings.dlssMode), 0, 1, DlssModeName((DlssMode)std::min(settings.dlssMode, 1u)));
		if (settings.dlssMode != prevMode) {
			const uint prevPreset = globals::features::upscaling.settings.presetDLSS;
			ClampPresetToMode();
			if (globals::features::upscaling.settings.presetDLSS != prevPreset) {
				logger::info("[FOVEATED] DLSS preset clamped from {} to {} after mode switch (J/K incompatible with Faster)",
					prevPreset, globals::features::upscaling.settings.presetDLSS);
			}
		}
		if (isFSR) {
			ImGui::EndDisabled();
			ImGui::TextWrapped(T(TKEY("foveated_dlss_mode_fsr_desc"), "Not used by FSR -- applies only when DLSS is the selected upscaler."));
		} else {
			switch (GetDlssMode()) {
			case DlssMode::kDefault:
				ImGui::TextWrapped(T(TKEY("foveated_dlss_mode_default_desc"), "Per-eye isolation: 5 copies per frame, 2 DLSS evaluates. All presets."));
				break;
			case DlssMode::kFaster:
				ImGui::TextWrapped(T(TKEY("foveated_dlss_mode_faster_desc"), "Viewport offset: 1 snapshot, 2 mask clears, 2 DLSS evaluates. Presets J/K unavailable."));
				break;
			default:
				break;
			}
		}

		ImGui::Separator();
		ImGui::Text("%s", T(TKEY("foveated_periphery_header"), "Periphery Rendering"));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("foveated_periphery_tooltip"),
								  "The area outside your selected subrect is filled cheaply rather than running\n"
								  "the selected upscaler. These settings control how that cheap fill looks and\n"
								  "whether it flickers.\n"
								  "\n"
								  "Stretch method: how pixels outside the subrect are reconstructed from the lower-res\n"
								  "render buffer. Does not affect the upscaled subrect region at all.\n"
								  "\n"
								  "Periphery AA: reduces temporal flicker in the stretched area using motion-compensated\n"
								  "history blending. Independent of the upscaled subrect.\n"
								  "\n"
								  "Edge Blend: controls how the upscaled subrect edge meets the stretched periphery.\n"
								  "Hard Copy leaves a sharp seam; Feather/Dither soften it. Only affects the boundary."));
		}

		ImGui::SliderInt(T(TKEY("foveated_stretch_label"), "Stretch"), reinterpret_cast<int*>(&settings.stretchMode), 0, 2, StretchModeName((StretchMode)settings.stretchMode));
		switch (GetStretchMode()) {
		case StretchMode::kBilinear:
			ImGui::TextWrapped(T(TKEY("foveated_stretch_bilinear_desc"), "Bilinear: smooth upscale of the render buffer. Looks soft but clean."));
			break;
		case StretchMode::kPoint:
			ImGui::TextWrapped(T(TKEY("foveated_stretch_point_desc"), "Point: cheapest, visibly pixelated. Good for benchmarking foveated savings."));
			break;
		case StretchMode::kGaussianBlur:
			ImGui::TextWrapped(T(TKEY("foveated_stretch_gaussian_desc"), "Gaussian: blurs the periphery further into soft focus. Good default for foveated use."));
			ImGui::SliderFloat(T(TKEY("foveated_blur_radius"), "Blur Radius"), &settings.peripheryBlurRadius, 0.5f, 4.0f, "%.1f px");
			break;
		}

		ImGui::SliderInt(T(TKEY("foveated_periphery_aa_label"), "Periphery AA"), reinterpret_cast<int*>(&settings.peripheryAAMode), 0, 1, PeripheryAAModeName((PeripheryAAMode)settings.peripheryAAMode));
		if (GetPeripheryAAMode() == PeripheryAAMode::kTemporalSmooth) {
			ImGui::TextWrapped(T(TKEY("foveated_periphery_aa_temporal_desc"), "Blends the stretched periphery with motion-reprojected history to reduce flicker."));
			ImGui::SliderFloat(T(TKEY("foveated_smoothing"), "Smoothing"), &settings.peripheryTemporalAlpha, 0.05f, 0.5f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("foveated_smoothing_tooltip"), "Lower = more temporal history (smoother but may ghost). Higher = more responsive."));
			}
		}

		ImGui::SliderInt(T(TKEY("foveated_edge_blend_label"), "Edge Blend"), reinterpret_cast<int*>(&settings.subrectBlendMode), 0, 2, SubrectBlendModeName((SubrectBlendMode)std::min(settings.subrectBlendMode, 2u)));
		switch (GetSubrectBlendMode()) {
		case SubrectBlendMode::kHardCopy:
			ImGui::TextWrapped(T(TKEY("foveated_blend_hard_copy_desc"), "Sharp seam at the subrect boundary. Lowest cost."));
			break;
		case SubrectBlendMode::kFeather:
			ImGui::TextWrapped(T(TKEY("foveated_blend_feather_desc"), "Smoothstep fade over N pixels at the boundary. Hides the seam."));
			ImGui::SliderFloat(T(TKEY("foveated_feather_width"), "Feather Width"), &settings.subrectFeatherWidth, 2.0f, 128.0f, "%.0f px");
			break;
		case SubrectBlendMode::kDither:
			ImGui::TextWrapped(T(TKEY("foveated_blend_dither_desc"), "Noise-dithered fade — more natural-looking than feather at large subrects."));
			ImGui::SliderFloat(T(TKEY("foveated_band_width"), "Band Width"), &settings.subrectFeatherWidth, 2.0f, 128.0f, "%.0f px");
			ImGui::SliderFloat(T(TKEY("foveated_noise_amount"), "Noise Amount"), &settings.subrectDitherStrength, 0.0f, 2.0f, "%.2f");
			break;
		}

		ImGui::Separator();
		ImGui::Text("%s", T(TKEY("foveated_subrect_region_header"), "Subrect Region"));
		ImGui::TextWrapped(T(TKEY("foveated_subrect_region_desc"),
			"Drag in the preview below to select the region that gets full upscaling. "
			"The rest is cheaply stretched — saves significant upscaling cost."));
		Util::Text::WrappedInfo(T(TKEY("foveated_screenshot_subrect_note"), "Screenshot has its own subrect; align them only if you want pixel-matched captures."));

		bool debugBool = settings.debugVisualize != 0;
		if (ImGui::Checkbox(T(TKEY("foveated_visualize_regions"), "Visualize regions"), &debugBool))
			settings.debugVisualize = debugBool ? 1u : 0u;
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("foveated_visualize_regions_tooltip"),
								  "Diagnostic: tint the cheap-stretched periphery red so the upscaled\n"
								  "subrect (un-tinted) pops visually in-game. Lets you confirm at a glance where\n"
								  "the selected upscaler is actually running vs where the cheap stretch is filling.\n"
								  "No perf impact; runtime toggle, no restart needed. Also shows briefly whenever\n"
								  "you drag-resize the region below, even with this off."));
		}

		// Preview off kVR_FRAMEBUFFER (the final composed SBS image the headset
		// sees) rather than kMAIN. kMAIN is mid-pipeline and carries non-1
		// alpha where Skyrim composited UI plates, so even with the opaque
		// blend callback you see the menu mask outline instead of the rendered
		// world. ScreenshotFeature picks the same RT for the same reason
		// (ScreenshotFeature.cpp:243). Foveated is VR-only so kVR_FRAMEBUFFER
		// is always populated when we get here.
		auto renderer = globals::game::renderer;
		if (renderer) {
			auto& fb = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kVR_FRAMEBUFFER];
			auto* tex = static_cast<ID3D11Texture2D*>(fb.texture);
			subrectController.DrawEditor(fb.SRV, tex, 0.5f, 0.0f, Util::Subrect::OpaquePreviewBlendCallback);
		} else {
			subrectController.DrawEditor(nullptr, nullptr, 0.5f);
		}

		if (subrectController.IsDragging())
			lastDragTime = std::chrono::steady_clock::now();

		// ── Eye Tracking (skeleton: mock source, debug overlay only) ──
		ImGui::Separator();
		ImGui::Text("%s", T(TKEY("foveated_eye_tracking_header"), "Eye Tracking Foveation"));
		ImGui::TextWrapped(T(TKEY("foveated_eye_tracking_desc"),
			"Development preview: a synthetic gaze source drives the pipeline. "
			"Use the debug overlay to see the tracked point move in the headset. "
			"Real headset eye tracking is not wired yet."));

		bool eyeTrackingBool = settings.eyeTrackingFoveationEnabled != 0;
		if (ImGui::Checkbox(T(TKEY("foveated_eye_tracking_enable"), "Enable Eye Tracking Pipeline"), &eyeTrackingBool))
			settings.eyeTrackingFoveationEnabled = eyeTrackingBool ? 1u : 0u;

		if (settings.eyeTrackingFoveationEnabled) {
			// Calibration bias: shift the tracked point until the crosshair
			// sits where you actually look when gazing straight ahead.
			ImGui::SliderFloat(T(TKEY("foveated_eye_tracking_bias_x"), "Calibration Bias X"),
				&settings.eyeTrackingBiasX, -0.5f, 0.5f, "%.3f");
			ImGui::SliderFloat(T(TKEY("foveated_eye_tracking_bias_y"), "Calibration Bias Y"),
				&settings.eyeTrackingBiasY, -0.5f, 0.5f, "%.3f");

			// FOV region movement: how far the gaze must drift before the
			// high-quality region repositions, and how fast it glides there.
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", "Deadzone around the anchored region center (pixels).\n"
					"The region re-centers only when the gaze drifts this far from it.\n"
					"Human gaze is never stationary, so keep this well above natural\n"
					"wander or the region churns constantly.");
			}
			ImGui::SliderFloat(T(TKEY("foveated_eye_tracking_fov_deadzone"), "FOV Deadzone"),
				&settings.eyeTrackingFovDeadzonePx, 50.0f, 800.0f, "%.0f px");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", "Glide speed: the region covers its remaining\n"
					"distance over ~1/factor frames once a move starts.\n"
					"Lower = faster arrival (pinhole reprojection keeps DLSS\n"
					"history stable); higher values glide more slowly.");
			}
			ImGui::SliderFloat(T(TKEY("foveated_eye_tracking_fov_glide"), "FOV Glide Speed"),
				&settings.eyeTrackingFovGlideFactor, 0.05f, 1.0f, "%.2f");

			const char* overlayModes[] = { "Off", "Crosshair", "Crosshair + Mask" };
			int overlay = static_cast<int>(settings.eyeTrackingDebugOverlay);
			if (ImGui::Combo(T(TKEY("foveated_eye_tracking_overlay"), "Debug Overlay"), &overlay, overlayModes, IM_ARRAYSIZE(overlayModes)))
				settings.eyeTrackingDebugOverlay = static_cast<uint>(overlay);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("foveated_eye_tracking_overlay_tooltip"),
									  "Crosshair: green cross at the tracked gaze point, per eye.\n"
									  "Crosshair + Mask: also darkens everything outside the future\n"
									  "foveated region around the gaze point."));
			}

			const char* sourceName = "None";
			switch (FoveatedRenderEyeTracking::GetLastGazeSource()) {
			case FoveatedRenderEyeTracking::GazeSource::kOpenVR:
				sourceName = "OpenVR (headset)";
				break;
			case FoveatedRenderEyeTracking::GazeSource::kMock:
				sourceName = "Mock (synthetic)";
				break;
			default:
				break;
			}
			ImGui::TextDisabled("Gaze source: %s | valid: %s", sourceName,
				eyeTrackingData.isValid ? "yes" : "no");
		}
	}
}

#undef I18N_KEY_PREFIX
