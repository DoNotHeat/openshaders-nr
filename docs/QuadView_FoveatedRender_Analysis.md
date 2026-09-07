# Quad View Foveated Rendering with Eye Tracking for Open Shaders

## Executive Summary

Implementing quad view foveated rendering with eye tracking in Open Shaders is **technically feasible** but requires significant architectural changes. The project already has a solid foundation:
- ✅ FoveatedRender system for subrect-based optimization
- ✅ VR support via the OpenVR API
- ✅ Framework for shader parameterization

Main challenges:
- ⚠️ Quad view requires 4 simultaneous viewports instead of the current 2
- ⚠️ Eye tracking data requires low-latency integration
- ⚠️ DLSS/FSR3 may require custom integration for 4 viewports
- ⚠️ Performance may be critical on target VR platforms

---

## Part 1: Current Architecture

### 1.1 Existing VR Structure

**Stereo buffers (Globals.h):**
```cpp
struct FrameBufferVR {
	DirectX::XMMATRIX CameraView[2];           // Per-eye view matrices
	DirectX::XMMATRIX CameraProj[2];           // Per-eye projection matrices
	// ... other matrices ...
};
```

**Current FoveatedRender:**
- Supports 2 viewports (left and right eye)
- Subrect is precomputed based on a fixed central region
- Periphery is stretched via `SubrectStretchCS.hlsl` (bilinear, point, or Gaussian blur)
- Foveation does NOT use eye tracking - only geometric centrality

### 1.2 OpenVR API Capabilities

**Current integration:**
- `OpenVRDetection.h` - detects the presence of an OpenVR runtime
- `BSOpenVR.h` - CommonLibSSE bindings to OpenVR structs
- Main access to: tracked device poses, controller input

**Required extensions for quad view:**

#### Eye Tracking Data
```cpp
// OpenVR provides eye tracking through the EyeTracking interface
EVRCompositorError Submit(EVREye eEye, const Texture_t *pTexture, 
						  const VRTextureBounds_t* pBounds = 0, 
						  EVRSubmitFlags nSubmitFlags = Submit_Default);

// Gaze data is obtained via:
vr::TrackedDeviceIndex_t hmdIndex = vr::k_unTrackedDeviceIndex_Hmd;
vr::EyeTrackingClientStatus status = 
	vrSystem->GetEyeTrackingClient(outEyeTrackingClient);
vr::TrackingResult result = 
	outEyeTrackingClient->GetEyeGazes(frameIndex, outGazeRays);
```

#### Quad View Support
OpenVR itself only supports 2 viewports (LEFT_EYE and RIGHT_EYE). **Quad view requires:**
- Using an extended OpenComposite or the Varjo-specific API
- OR synthesizing quad view from 2 viewports via subrect compositing

**Recommendation:** Quad view is best implemented as an **internal extension**, not via OpenVR submit

---

## Part 2: Required Architectural Changes

### 2.1 Globals.h Extension

**Add quad view buffers:**

```cpp
struct FrameBufferVRQuad {
	// Quad layout:  TL  TR
	//               BL  BR
	// (TopLeft, TopRight, BottomLeft, BottomRight - each at half resolution)

	DirectX::XMMATRIX CameraView[4];           // [0]=TopLeft, [1]=TopRight, [2]=BottomLeft, [3]=BottomRight
	DirectX::XMMATRIX CameraProj[4];
	DirectX::XMMATRIX CameraViewProj[4];
	DirectX::XMMATRIX CameraViewProjUnjittered[4];
	// ... other per-quad matrices ...
};

// Add to SharedDataCB:
struct {
	FrameBuffer nonVR;
	FrameBufferVR vr;
	FrameBufferVRQuad vrQuad;  // NEW

	// Mode enum:
	uint vrMode;  // 0=Disabled, 1=Stereo(2-view), 2=Quad(4-view)
};
```

### 2.2 Eye Tracking Data Structure

**New struct for VR.h:**

```cpp
// Eye tracking input - obtained from OpenVR
struct EyeTrackingData {
	bool isValid;           // true if eye tracking is active
	DirectX::XMFLOAT3 gazeOriginLeft;     // World-space gaze ray origin (left eye)
	DirectX::XMFLOAT3 gazeOriginRight;    // World-space gaze ray origin (right eye)
	DirectX::XMFLOAT3 gazeDirectionLeft;  // Normalized gaze direction (left eye)
	DirectX::XMFLOAT3 gazeDirectionRight; // Normalized gaze direction (right eye)

	// Computed foveation center for each viewport (in 0..1 NDC)
	DirectX::XMFLOAT2 foveationCenterQuad[4];  // Quad viewport foveation centers
};
```

### 2.3 FoveatedRender Extension

**Add to FoveatedRender::Settings:**

```cpp
struct Settings {
	// ... existing fields ...

	// Quad View settings
	uint quadViewEnabled = 0;              // Opt-in quad view rendering
	uint eyeTrackingFoveationEnabled = 0;  // Dynamic gaze-based foveation
	float eyeTrackingLatencyMs = 8.0f;     // Eye tracker latency for prediction

	// Per-quadrant DLSS presets (may differ from the stereo suggestion)
	uint quadDlssMode[4] = {
		(uint)DlssMode::kDefault,  // TopLeft
		(uint)DlssMode::kDefault,  // TopRight
		(uint)DlssMode::kDefault,  // BottomLeft
		(uint)DlssMode::kDefault   // BottomRight
	};

	// Quad-specific blending and smoothing
	float quadBlendFeatherWidth = 32.0f;    // Smoother transitions between quads
	uint quadTemporalSmoothingFrames = 3;   // Eye tracking jitter smoothing
};

struct FoveationProfileQuad {
	bool available = false;

	// Foveation profile for each quad viewport
	struct QuadProfile {
		float2 gazePoint;           // NDC space (-1..1)  
		float coverageScale;        // 0..1 - how much of the quad is rendered at full res
		float centerHorizontalScale; // 1..2 for asymmetric stretching
	} quadProfiles[4];
};
```

---

## Part 3: Required Code Changes

### 3.1 Main Components

#### A. VR.h - Eye Tracking Integration

```cpp
struct VR : OverlayFeature {
	// Get the current eye tracking data
	bool GetEyeTrackingData(EyeTrackingData& outData);

	// Update foveation centers based on gaze + quad layout
	void UpdateQuadViewFoveationCenters(
		const EyeTrackingData& eyeTracking,
		FoveatedRender::FoveationProfileQuad& outProfile);

	// Quad view mode control
	void SetQuadViewEnabled(bool enabled);
	bool IsQuadViewActive() const;

private:
	EyeTrackingData currentEyeTracking{};
	std::chrono::steady_clock::time_point lastEyeTrackingTime;
};
```

#### B. FoveatedRender.cpp - Quad View Logic

```cpp
bool FoveatedRender::IsQuadViewActive() const {
	if (!settings.quadViewEnabled) return false;
	if (!globals::game::isVR) return false;

	auto* vr = VR::GetSingleton();
	return vr && vr->IsQuadViewActive();
}

void FoveatedRender::EvaluateQuadView(
	const EyeTrackingData& eyeTracking) {

	// Compute quad viewport layout (each quad at half viewport resolution)
	// with appropriate field-of-view adjustment (wider FOV per quad)

	// Update foveation profiles based on gaze
	auto foveationProfile = GetFoveationProfileQuad();

	// Dispatch separate DLSS evaluations for each quad
	// (or composite the quads into a single 1x1 SBS if DLSS does not support it)

	// Update subrect stretch passes to work in quad coordinates
}
```

#### C. Shader Changes

**FoveatedMask.hlsli** - Parametrize for 4 viewports:

```hlsl
// Sample quad-specific foveation center
float4 GetFoveationMask_Quad(
	float2 uv,           // Quad-local NDC coords
	uint quadIndex,      // 0-3 for quad viewport
	float2 gazeCenter,   // Eye tracking derived gaze point
	float coverageScale) // Settings-driven coverage
{
	// Compute the subrect mask for the specific quad
	// The gaze center can be used to dynamically offset
	// the mask based on eye tracking
}

// SubrectStretchCS.hlsl updated for 4 viewports
[numthreads(8, 8, 1)]
void CS_StretchQuad(uint3 dispatchID : SV_DispatchThreadID) {
	uint quadIndex = dispatchID.z;  // 0-3

	// Read from quad-local high-res DLSS output
	// Stretch the periphery relative to that quad's gaze center
	// Write to the swapchain quad quadrant
}
```

---

## Part 4: Performance Considerations

### 4.1 Memory Impact

| Metric | Stereo (2) | Quad (4) | Ratio |
|--------|------------|----------|-------|
| View matrices | 2 | 4 | 2.0x |
| RT allocations | 1x | 0.5x per quad | ~1.5x total |
| Bandwidth | 1x | ~1.8x | ⚠️ May become a bottleneck |
| CB updates | 1x | 1x (batched) | 1.0x |

### 4.2 Rendering Pipeline Impact

**Quad view requires:**
1. ✅ 4x viewport computations (matrices) - cheap
2. ✅ Geometry processing reuse - same scene graph
3. ⚠️ 4x DLSS evaluate calls (or 1 composite if a custom subrect is used)
4. ⚠️ More complex bandwidth during compositor upscaling

**Recommendation:** 
- Use DlssMode::kFaster if possible (Streamline SBS-subrect)
- Fall back to DlssMode::kDefault with 2 quads at a time over 2 frames if necessary

### 4.3 Eye Tracking Latency

Eye tracking is typically 8-12ms behind the rendering frame. **Required:**
- Predictive gaze estimation based on eye velocity
- Temporal filtering to smooth jitter
- Fallback to a fixed center if tracking is lost

---

## Part 5: Implementation Roadmap

### Phase 1: Foundation (2-3 weeks)
- [ ] Extend Globals.h for quad view matrices
- [ ] Add EyeTrackingData struct  
- [ ] Implement GetEyeTrackingData() in VR.h with OpenVR
- [ ] Simple quad layout (no eye tracking, fixed foveation centers)

### Phase 2: Foveation (2-3 weeks)  
- [ ] Parametrize the shaders for 4 viewports
- [ ] Implement quad-specific subrect stretching
- [ ] Add the eye tracking gaze-to-NDC transform
- [ ] Temporal smoothing for eye tracking

### Phase 3: DLSS Integration (2-3 weeks)
- [ ] Evaluate DLSS with 4 viewports
- [ ] Implement fallback to 2x2 quad compositing if DLSS limits are hit
- [ ] Performance profiling and preset tuning

### Phase 4: Polish & Testing (1-2 weeks)
- [ ] UI for mode switching (Stereo ↔ Quad)
- [ ] Devbench actions for quad view scenarios
- [ ] Cross-platform testing (Varjo, HP Reverb G2, etc)

---

## Part 6: Risk Assessment

### 6.1 High Risks

| Risk | Mitigation |
|------|-----------|
| DLSS does not support 4 simultaneous evaluates | Use 2x2 quad compositing with 2 DLSS evaluates per frame |
| Eye tracking jitter causes visual strobing | Aggressive temporal filtering (Kalman filter on the gaze point) |
| Bandwidth saturated with quad + DLSS | Reduce the DLSS preset quality for the quad edge viewports |

### 6.2 Medium Risks

| Risk | Mitigation |
|------|-----------|
| VR runtime availability varies | Graceful fallback to stereo if quad is unavailable |
| Performance regression in stereo mode | Conditional compilation - separate quad code path |
| Eye tracking calibration drift | Per-user calibration store in settings |

### 6.3 Testing Strategy

```
1. Unit tests for the transform matrices (quad NDC -> gaze point)
2. Visual tests in stereo mode (must match current behavior)
3. Quad mode on every supported VR headset:
   - Varjo XR-4 (reference)
   - HP Reverb G2 (with adapter)
   - Meta Quest Pro (if eye tracking available)
4. Performance baseline: target 90fps @ maximum settings
```

---

## Part 7: Compatibility Matrix

### VR Runtime & Headset Support

| Headset | OpenVR | Quad View | Eye Tracking | Notes |
|---------|--------|-----------|--------------|-------|
| Varjo XR-4 | ✅ | ✅ | ✅ | Reference platform |
| HP Reverb G2 Omnicept | ✅ | ❌ | ✅ | Requires adapter |
| Meta Quest Pro | ⚠️ | ❌ | ✅ | No OpenVR support |
| HTC Vive Pro Eye | ✅ | ❌ | ✅ | Max 2048x2048 per eye |
| Pimax 8K X | ✅ | ❌ | ⚠️ | Requires Pimax SDK |

**Conclusion:** Quad view has a limited audience (mostly Varjo). Recommended as an **optional premium feature** with graceful fallback.

---

## Part 8: Code Example: Quad View Stereo Blending

```cpp
// Example shader to composite the quads back into a stereo output
// (if the VR compositor requires a stereo submit)

Texture2D<float4> QuadTextures[4] : register(t0);  // TL, TR, BL, BR
RWTexture2D<float4> StereoOutput : register(u0);   // Stereo render target

[numthreads(16, 16, 1)]
void CS_CompositeQuadToStereo(uint3 dispatchID : SV_DispatchThreadID) {
	uint2 stereoCoord = dispatchID.xy;
	uint eyeIndex = (stereoCoord.x >= 1280) ? 1 : 0;  // SBS layout

	// Map SBS coord to quad coordinates
	uint2 quadCoord = (eyeIndex == 0) 
		? stereoCoord 
		: uint2(stereoCoord.x - 1280, stereoCoord.y);

	// Quad layout in half-res:  TL TOP-HALF    TR TOP-HALF
	//                           BL BOT-HALF    BR BOT-HALF
	uint quadIndex = (quadCoord.y < 540) ? 0 : 2;  // T or B
	if (quadCoord.x >= 960) quadIndex++;             // L or R

	float4 color = QuadTextures[quadIndex].Load(
		uint3(quadCoord % 960, 0));  // Modulo for quad-local coords

	StereoOutput[stereoCoord] = color;
}
```

---

## Part 9: Decision Criteria

### ✅ Go Forward if:
1. Your target headset has **official quad view support** (Varjo XR-4)
2. You can **test on the target device**
3. The **performance budget** allows a 1.5-2.0x RT bandwidth increase
4. This is a **differentiating feature** for your application

### ⏸️ Consider Later if:
1. Headsets evolve and broader support appears
2. DLSS5/FSR4 adds official quad viewport support
3. Eye tracking becomes more widespread

### ❌ Not Recommended if:
1. You target **Meta Quest or WMR** (no quad view support)
2. **100% backward compatibility** is required with no performance regression
3. Your project is already **budget-constrained** on VR performance

---

## Final Recommendations

### Approach 1: **Minimum Viable Quad View** (recommended starting point)
- ✅ Implement quad view as an **optical illusion** via subrect compositing
- ✅ Use the existing FoveatedRender subrect infrastructure
- ✅ No 4x DLSS evaluates required - only 2
- ⏱️ Estimate: 2-3 weeks of development
- 📊 Performance impact: ~10-15% in quad mode

### Approach 2: **Full Native Quad View**  
- 🎯 Requires extending DLSS to 4 viewports
- 📊 Performance impact: ~30-50% in quad mode
- ⏱️ Estimate: 4-6 weeks of development
- ⚠️ **Requires active testing on the target DevKit**

### Approach 3: **Hybrid Eye Tracking Foveation**
- Keep the existing stereo 2-viewport mode
- Add only **dynamic foveation** based on eye tracking
- 📊 Performance impact: ~5-10% overhead for tracking processing
- ⏱️ Estimate: 1-2 weeks of development
- ✅ **The most practical short-term choice**

---

## References and Resources

1. **OpenVR Eye Tracking API:**
   - `openvr/headers/openvr.h` - `IVRSystem::GetEyeTrackingClient()`
   - Varjo API docs - https://developer.varjo.com/

2. **Project References:**
- `src/Features/Upscaling/FoveatedRender.h` - Current implementation
   - `src/Globals.h` - FrameBufferVR structurc
- `features/Upscaling/Shaders/Upscaling/FoveatedRender/` - HLSL shaders

3. **Related Features in Open Shaders:**
   - VRStereoOptimizations - Existing VR optimizations
   - Util::Subrect::Controller - Subrect control logic
