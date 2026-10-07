# Porting Eye-Tracking Foveation — Plan for the Porting AI

Goal: port gaze-following foveation (VR eye-tracking FOV) from this repo into a target repo. The porting AI receives the source files listed below; adapt them to the target repo's architecture.

## Architecture Overview

Per-frame flow:

```
EyeTrackingVR.h (gaze source: OpenVR / mock)
  → EyeTrackingFoveation.h (math utils)
  → FoveatedRender::UpdateEyeTrackingFoveation() (state machine)
  → Subrect::Controller::SetGazeOffset() (moves the DLSS crop)
  → Streamline.cpp (pinhole reprojection of DLSS history)
  → NeuralRendering/Integration.cpp (mvec compensation + intensity ramp)
  → SubrectStretchCS.hlsl (debug overlay)
```

## Files to Port (source → purpose)

### 1. Gaze acquisition — `src/Features/VR/EyeTrackingVR.h`
- `namespace FoveatedRenderEyeTracking`: `EyeTrackingDataVR`, `GazeSource` enum, `vr026::IVRSystem` (minimal OpenVR 2.15.x vtable up to `GetEyeTrackedFoveationCenter`, slot 35).
- `TryGetOpenVREyeTracking()` — resolves `IVRSystem_026` from `openvr_api.dll` via `VR_GetGenericInterface`, caches it, reads per-eye NDC foveation centers.
- `TryGetEyeTrackingData()` — source selection: real tracker → hold-last-gaze on failure (45 frames) → Lissajous mock fallback.
- `GetLastGazeSource()` — for UI status.
- Port as-is; only dependency is `logger` and `windows.h`.

### 2. Math utils — `src/Features/Upscaling/EyeTrackingFoveation.h`
- `EyeTrackingData` struct (per-eye raw + smoothed NDC gaze).
- `SmoothGazePoint` (EMA), `ClampGazeToValidRegion` (±0.4 NDC), `GazeToSubrectOffset` (NDC → pixel/UV offset).
- Self-contained, no deps.

### 3. Core state machine — `src/Features/Upscaling/FoveatedRender.cpp` → `FoveatedRender::UpdateEyeTrackingFoveation()` (line ~261)
Port this method + its state fields from `FoveatedRender.h`:
- Settings (FoveatedRender.h:164-196): `eyeTrackingFoveationEnabled`, `eyeTrackingDebugOverlay`, `eyeTrackingBiasX/Y`, `eyeTrackingFovDeadzonePx`, `eyeTrackingFovTargetSmoothing`, `eyeTrackingFovGlideFactor`, `eyeTrackingGazeRampEnabled/Seconds/MinIntensity`.
- State (FoveatedRender.h:214-280): `eyeTrackingData`, `lastGazeOffsetUV[2]`, `lastGazeOffsetRightUV[2]`, `gazeTargetUV[2][2]`, `gazeOverThresholdFrames[2]`, `gazeGlideActive`, `gazeReturnActive` + timers, `lastSubrectDeltaUV[2][2]`, `subrectMovedThisFrame`, `GetGazeOffsetNDC(eyeIndex)`.
- Logic stages: bias subtract → EMA smoothing (alpha 0.2) → `GazeToSubrectOffset` → target EMA (user alpha) → 8px quantization → deadzone + 8-frame dwell + saccade bypass (40px/frame) → binocular lock (both eyes glide together) → glide toward target → gaze-loss return-to-center (hold 0.1s, ease 0.15s).
- Called every frame from `FoveatedRender/Modes.cpp:63` (`ExecuteFoveatedRoute`, before any pass reads gaze).

### 4. Crop movement — `src/Utils/Subrect.cpp` / `Subrect.h`
- `Controller::SetGazeOffset(x, y, rightX, rightY)` (Subrect.cpp:474): applies per-eye offset on top of base crop, never accumulates, clamps to [0, 1-w]. `GetUV()`/`GetRightEyeUV()` return the gaze-shifted region when active.
- Port the `gazeOffset*`/`gazeShiftedUV` fields (Subrect.h:259-264) into the target's crop controller.

### 5. DLSS history reprojection — `src/Features/Upscaling/Streamline.cpp` (~line 481)
- In `CheckFrameConstants` VR branch: fold `GetGazeOffsetNDC(eye)` into `currViewProj` and last frame's offset into `prevViewProj` (`m[0].w += off.x; m[1].w -= off.y`). This makes DLSS reproject temporal history across crop moves instead of resetting.

### 6. Neural Rendering integration — `src/Features/Upscaling/NeuralRendering/Integration.cpp`
- Gaze-shift intensity ramp (line ~472): when `subrectMovedThisFrame && eyeTrackingGazeRampEnabled`, ease NR intensity from `eyeTrackingGazeRampMinIntensity` over `eyeTrackingGazeRampSeconds`.
- Crop-motion compensation (line ~600): `lastSubrectDeltaUV[eye]` → `cropMotionOffsetX/Y` (Y negated), adaptive reset threshold `max(64px, 12.5% of crop min extent)` → `resetHistory`.
- Also `FoveatedRender/Modes.cpp:191` + `Core.cpp:973` `CropMotionCompensate()` (rewrites cropped mvec via `CropMotionCS.hlsl`) and `Params.cpp:82` (deltas into `VRDlssParams`).

### 7. Debug overlay — `features/Upscaling/Shaders/Upscaling/FoveatedRender/SubrectStretchCS.hlsl`
- CB fields `GazeX/GazeY/GazeDebugMode` (0=off, 1=crosshair, 2=crosshair+vignette). Drawn in the stretch pass.
- C++ side: `FoveatedRender/Core.cpp` `StretchCB` struct (line ~100) + fill at line ~498 (reads `eyeTrackingData`, `eyeTrackingDebugOverlay`).

### 8. UI — `FoveatedRender.cpp` `DrawSettings()` (line ~1044)
- Enable checkbox, bias sliders, deadzone/target-smoothing/glide sliders, gaze-ramp controls, overlay combo, live "Gaze source" readout.

## Porting Order (recommended)

1. `EyeTrackingVR.h` + `EyeTrackingFoveation.h` (no deps) → compile.
2. Settings + state fields + `UpdateEyeTrackingFoveation()` + per-frame call.
3. `SetGazeOffset` in crop controller.
4. Streamline pinhole reprojection (DLSS path).
5. Neural Rendering ramp + mvec compensation.
6. Debug overlay (HLSL + CB).
7. UI.

## Notes / Gotchas

- VR-only feature: gate on `isVR`; non-VR must be unaffected.
- Gaze NDC y is up; UV y is down → negate Y when applying offsets.
- Subrect UV hash (`ComputeSubrectUVHash`) must hash only size+mode, NOT position — position changes every frame under gaze.
- `SetGazeOffset(0,0,0,0)` clears the offset (used when disabled / gaze lost).
- Mock Lissajous source is dev-only; never reached once real hardware is seen.
- Name all D3D11 resources with `Util::SetResourceName`; wrap pass entry points with `CS_GPU_PASS`.