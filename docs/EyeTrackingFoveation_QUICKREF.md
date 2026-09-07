# ⚡ Quick Reference - Eye Tracking Foveation

## 📋 In 2 Minutes

### Problem
You want the high-resolution region (subrect) to **follow the user's gaze** via eye tracking.

### Solution
Simply add an eye tracking offset to the existing foveation system.

---

## 🔧 3 Integration Steps

### Step 1: Add the includes
```cpp
// FoveatedRender.cpp
#include "EyeTrackingFoveation.h"
#include "VR/EyeTrackingVR.h"
#include "EyeTrackingFoveationImpl.h"
```

### Step 2: Add one line of code
```cpp
// In your rendering function (where foveationProfile is computed)

auto profile = GetFoveationProfile();  // ← Existing code

// ↓↓↓ ADD THIS LINE ↓↓↓
if (globals::game::isVR && settings.eyeTrackingFoveationEnabled)
	FoveatedRenderImpl::ApplyEyeTrackingOffsetToFoveation(profile, settings, eyeTrackingData);
// ↑↑↑ HERE ↑↑↑

// Then use profile as usual
```

### Step 3: Update the HLSL
```hlsl
// FoveatedMask.hlsli - in the mask calculation function

// Before:
// float2 center = float2(0.0, 0.0);

// After:
float2 eyeTrackingOffset = GetEyeTrackingOffset(eyeIndex);  // From the constant buffer
float2 center = GetFoveationCenterWithEyeTracking(eyeIndex, eyeTrackingOffset);
```

**Done!** 🎉 Foveation now follows the gaze.

---

## 🎮 User Settings (ImGui)

All three parameters appear in the menu automatically:

```
☑️ Foveated DLSS
  Menu items:
  ☑️ Eye Tracking Foveation
	 Gaze Smoothing: [===◆====]  0.2
	 Gaze Threshold: [=◆=======]  2.0
```

---

## ⚙️ Tuning Parameters

| Parameter | Value | Effect |
|-----------|-------|--------|
| `eyeTrackingFoveationEnabled` | 0 or 1 | Enable/disable |
| `eyeTrackingGazeSmoothingAlpha` | 0.0-1.0 | Higher = smoother, lower = more responsive |
| `eyeTrackingGazeThresholdPixels` | 0.0-10.0 | Deadzone: minimum movement |

**Recommended values:** 0.2 smoothing, 2.0 threshold

---

## 🔍 If Something Does Not Work

| Question | Answer |
|---------|--------|
| Foveation not moving? | 1) Is the option enabled in the menu? 2) Is eye tracking calibrated? |
| Too jittery? | Increase smoothing (0.3-0.4) or threshold (3-4) |
| Too slow? | Decrease smoothing (0.1) or threshold (1) |
| Eye tracking not seeing data? | Check whether your VR headset supports eye tracking |

---

## 📊 Quick Facts

- **Overhead:** ~0.6ms (negligible)
- **Support:** Varjo, HP Reverb G2, HTC Vive Pro Eye, and 5 more headsets
- **Fallback:** If eye tracking is unavailable, it simply stays at the center
- **Compatibility:** 100% compatible with the existing FoveatedRender
- **JSON:** Automatically saved/loaded

---

## 🎯 Expected Result

```
👁️ The user looks to the right
	↓
🔄 The eye tracker sends coordinates
	↓
📍 FoveatedRender computes the offset
	↓
🎨 The shader draws the subrect (high resolution) to the right
	↓
✨ The user immediately sees maximum quality where they are looking!
```

---

## 📁 Files to Read (in order)

1. **IF IN A HURRY:** 👈 You are here (Quick Reference) - 2 min
2. **FULL GUIDE:** `docs/EyeTrackingFoveation_Implementation_Guide.md` - 10 min
3. **CODE:** `src/Features/Upscaling/EyeTrackingFoveation.h` - for details
4. **HLSL EXAMPLES:** `features/Upscaling/Shaders/Upscaling/FoveatedRender/EyeTrackingFoveation.hlsli`

---

**Questions?** See the full documentation or the code comments in the header files.
