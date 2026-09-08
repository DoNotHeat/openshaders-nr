// ============================================================================
// NeuralPreScaleCS — downsample DLSSNR inputs to the Neural Quality resolution
// ============================================================================
//
// The Neural Quality setting evaluates DLSSNR at a reduced resolution. NGX
// must see the WHOLE foveal subrect at that resolution — cropping the top-left
// corner (CopySubresourceRegion) would feed the network a zoomed fragment and
// desync it from the guides (temporal flicker). This pass properly downsamples:
//
//   color    — kTOTAL SBS, bilinear at the subrect's normalized UV
//   depth    — subrect guide, point-sampled (no blending across surfaces)
//   mvec     — subrect guide, point-sampled (per-texel motion is preserved)
//
// One dispatch per eye; outputs are evalW x evalH textures that replace the
// cropped inputs in Renderer::ApplyStereo.
// ============================================================================

cbuffer PreScaleParams : register(b0)
{
	float2 subrectOriginUV;  // subrect origin in kTOTAL UV (per eye)
	float2 subrectSizeUV;    // subrect size in kTOTAL UV (same both eyes)
};

Texture2D<float4> SrcColor : register(t0);  // kTOTAL SBS
Texture2D<float4> SrcDepth : register(t1);  // subrect-size depth guide
Texture2D<float4> SrcMvec : register(t2);   // subrect-size motion vector guide

RWTexture2D<float4> DstColor : register(u0);
RWTexture2D<float> DstDepth : register(u1);
RWTexture2D<float4> DstMvec : register(u2);

SamplerState LinearSampler : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint2 size;
	DstColor.GetDimensions(size.x, size.y);
	if (dispatchThreadID.x >= size.x || dispatchThreadID.y >= size.y)
		return;

	const float2 uv = (dispatchThreadID.xy + 0.5) / size;
	DstColor[dispatchThreadID.xy] = SrcColor.SampleLevel(LinearSampler, subrectOriginUV + uv * subrectSizeUV, 0);

	// Guides: point-sample the source texel this eval texel falls into. Depth
	// must not be blended across surfaces; motion vectors stay per-texel.
	uint2 srcSize;
	SrcDepth.GetDimensions(srcSize.x, srcSize.y);
	const uint2 srcPix = min(uint2(uv * srcSize), srcSize - 1);
	DstDepth[dispatchThreadID.xy] = SrcDepth[srcPix].x;
	DstMvec[dispatchThreadID.xy] = SrcMvec[srcPix];
}
