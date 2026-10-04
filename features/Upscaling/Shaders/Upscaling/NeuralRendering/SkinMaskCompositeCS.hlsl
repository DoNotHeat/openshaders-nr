// ============================================================================
// SkinMaskCompositeCS — gated DLSS Neural Rendering write-back
// ============================================================================
//
// DLSSNR (NGX Feature 18) evaluates the whole region it is given (flat
// framebuffer or VR foveated subrect), but the desired behaviour is to keep
// its output ONLY on character pixels. This pass blends the evaluated color
// with the original frame using the character mask written by the lighting
// pass:
//
//   Masks.y == 1.0   character geometry (face, skin, clothing, hair, armour)
//                    -> full DLSSNR output
//   Masks.y == 0.0   everything else     -> original color
//
// A soft falloff around the 0.25 threshold feathers the boundary so the
// composite edge is not a hard cut. The original color is provided by the
// caller because the DLSSNR output buffer already overwrote it by the time
// this pass runs (Integration.cpp keeps a pristine copy).
//
// Register layout mirrors CopyDepthGuideCS.hlsl (single SRV + single UAV,
// all other resources unbound) so the integration can bind it standalone.
// ============================================================================

Texture2D<float4> OriginalColor : register(t0);   // pristine pre-DLSSNR frame
Texture2D<float4> NeuralColor : register(t1);     // DLSSNR evaluated output
Texture2D<float3> FaceMask : register(t2);	// GBuffer MASKS (kRAWINDIRECT_PREVIOUS)

RWTexture2D<float4> CompositeColor : register(u0);

SamplerState MaskSampler : register(s0);

cbuffer SkinMaskParams : register(b0)
{
	// Normalized render/display mapping: maskUV = pixelUV * maskScale. Both the
	// mask (render-res SBS GBuffer) and the frame (display-res SBS) start at the
	// same origin (0,0), so no per-eye/per-subrect offset is needed — the region
	// being composited is the whole frame and the pixels the effect actually hit
	// are already restricted to the foveal subrects by the DLSSNR write-back.
	float2 maskScale;     // renderPixels per displayPixel (maskW/frameW, maskH/frameH)
	float rampScale;      // weight = saturate(mask * rampScale). 2.0 maps 0->0, 0.5 (beast
			      // face)->1, 1 (human face / non-FaceGen character)->1, and spreads
			      // bilinear edge intermediates (0..0.5) into a soft feather; lower =
			      // wider feather.
	float maskErodePx;    // erode the mask by this many display pixels before blending.
			      // The raw binary mask's bilinear edge gradient shifts with camera
			      // motion, so boundary pixels oscillate between original and neural
			      // output (shimmering silhouette). Erosion pulls the blend edge
			      // inside the character onto stable pixels; the silhouette itself
			      // stays original. 0 disables.
	uint debugVisualize;  // dev: output the mask weight instead of the composite
	float pad2;
	// Neural render-scale: the DLSSNR result for each eye's foveal subrect lands
	// in a proportionally-sized box at the subrect origin (whole subrect in
	// miniature). The composite bilinearly stretches it back over the subrect:
	// neuralUV = boxOrigin(eye) + localUV * subrectSize * boxScale. Scale 1.0 =
	// identity (1:1 sample). Origins/scales are normalized frame UVs, computed
	// CPU-side from the subrect UVs.
	float2 neuralBoxScale;
	float neuralBoxOffsetX;       // left-eye box origin X in frame UV
	float neuralBoxOffsetY;       // box origin Y in frame UV (same both eyes)
	float neuralBoxOffsetXRight;  // right-eye box origin X in frame UV
	float pad3;
};

float MaskValue(uint2 colorPixel, uint2 maskDims)
{
	// Map a display-res frame pixel to the render-res mask via the normalized
	// scale (both SBS spaces share origin 0,0), then bilinear-read the mask.
	float2 maskUV = (colorPixel + 0.5) * maskScale / maskDims;
	return FaceMask.SampleLevel(MaskSampler, maskUV, 0).y;
}

// Eroded mask sample: the minimum of the bilinear mask over a cross of
// neighbors spanning erodePx display pixels. A binary mask's bilinear edge
// gradient is only ~1px wide, so a boundary pixel's weight oscillates between
// original and neural output as the camera moves — a shimmering silhouette.
// Taking the min over the cross pulls the blend edge inside the character onto
// stable pixels; the silhouette itself stays original. The cross (not a box)
// keeps the cost at 5 samples regardless of erodePx.
float ErodedMaskValue(uint2 colorPixel, uint2 maskDims)
{
	if (maskErodePx <= 0.0f)
		return MaskValue(colorPixel, maskDims);

	const float2 step = float2(maskErodePx, maskErodePx);
	// int2 arithmetic so negative offsets stay valid; SampleLevel clamps the
	// resulting UV to the texture edge (CLAMP addressing), so out-of-bounds
	// neighbors read the border mask value instead of wrapping.
	const int2 center = int2(colorPixel);
	float m = MaskValue(colorPixel, maskDims);
	m = min(m, MaskValue(uint2(center + int2(step.x, 0)), maskDims));
	m = min(m, MaskValue(uint2(center - int2(step.x, 0)), maskDims));
	m = min(m, MaskValue(uint2(center + int2(0, step.y)), maskDims));
	m = min(m, MaskValue(uint2(center - int2(0, step.y)), maskDims));
	return m;
}

float4 NeuralSample(uint2 colorPixel, uint2 frameDims)
{
	if (neuralBoxScale.x >= 0.999 && neuralBoxScale.y >= 0.999)
		return NeuralColor[colorPixel];

	// SBS: which eye does this pixel belong to, and where inside that eye?
	const float eyeW = frameDims.x * 0.5;
	const uint eye = colorPixel.x >= eyeW ? 1 : 0;
	const float eyeOriginX = eye * eyeW;
	const float2 eyeSize = float2(eyeW, frameDims.y);
	const float2 local = (float2(colorPixel) - float2(eyeOriginX, 0)) / eyeSize;
	// The neural box is the whole subrect in miniature at the subrect origin;
	// stretch it back over the full subrect.
	const float2 boxOriginPix = float2(eye ? neuralBoxOffsetXRight : neuralBoxOffsetX, neuralBoxOffsetY) * float2(frameDims);
	const float2 boxUV = (boxOriginPix + local * eyeSize * neuralBoxScale) / float2(frameDims);
	return NeuralColor.SampleLevel(MaskSampler, boxUV, 0);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint2 size;
	CompositeColor.GetDimensions(size.x, size.y);
	if (dispatchThreadID.x >= size.x || dispatchThreadID.y >= size.y)
		return;

	uint2 maskDims;
	FaceMask.GetDimensions(maskDims.x, maskDims.y);

	// Eroded bilinear sample — 5 samples per pixel (center + cross), not a full
	// neighborhood loop. The character-mask boundary sits on the silhouette where
	// the transition is far less visible than it was on the face outline, and the
	// erosion + ramp already give a soft edge inside the character. A per-pixel
	// erosion/dilation loop over the full frame (up to ~28 samples per pixel x
	// ~33M pixels in VR) costs several ms and is not needed.
	float mask = ErodedMaskValue(dispatchThreadID.xy, maskDims);

	float weight = saturate(mask * max(rampScale, 1e-4));

	if (debugVisualize) {
		CompositeColor[dispatchThreadID.xy] = float4(0.0, weight, 0.0, 1.0);
		return;
	}

	float4 original = OriginalColor[dispatchThreadID.xy];
	float4 neural = NeuralSample(dispatchThreadID.xy, size);

	CompositeColor[dispatchThreadID.xy] = lerp(original, neural, weight);
}
