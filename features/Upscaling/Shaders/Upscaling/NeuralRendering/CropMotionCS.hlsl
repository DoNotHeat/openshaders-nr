// ============================================================================
// CropMotionCS — motion-vector compensation for a moving NR subrect
// ============================================================================
//
// The gaze-following foveal subrect moves between frames, and the neural
// rendering (NGX Feature 18) temporal history is tied to the crop window:
// after a move, the network sees a shifted view with unchanged motion
// vectors and tracks phantom motion. Instead of resetting the history,
// this pass writes a private copy of the motion-vector crop with the
// subrect's own movement added to every vector:
//
//   previousLocal = currentLocal + sceneMotion + currentOrigin - prevOrigin
//
// so the network reprojects its history across the subrect move as if the
// crop had been stationary. Point sampling preserves distinct object
// velocities and invalid markers; blending across a silhouette would
// invent a third, incorrect velocity (CheekyFoveatedDLSS approach).
//
// Constants are in the C++ CropMotionCB layout (Integration.cpp).
// ============================================================================

Texture2DArray<float2> Source : register(t0);
RWTexture2D<float2> Destination : register(u0);
cbuffer CropMotionCB : register(b0) {
	uint2 Base;      // crop origin in the full SBS mvec texture (pixels)
	uint2 Size;      // crop size = output dispatch size (pixels)
	float2 Offset;   // subrect movement delta in mvec-vector units
	uint2 SourceSize; // full SBS mvec texture size (pixels)
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= Size)) return;
	// Point sampling preserves distinct object velocities and invalid markers;
	// blending across a silhouette would invent a third, incorrect velocity.
	const uint2 source_pixel = min(uint2((float2(id.xy) + 0.5) *
		float2(SourceSize) / float2(Size)), SourceSize - 1U);
	const float2 mv = Source.Load(int4(Base + source_pixel, 0, 0));
	// Output-space displacement scales along with the output grid. Offset is
	// expressed in the original stored-vector units before this conversion.
	Destination[id.xy] = any(!isfinite(mv)) || any(abs(mv) > 1e15)
		? mv : (mv + Offset) * float2(Size) / float2(SourceSize);
}
