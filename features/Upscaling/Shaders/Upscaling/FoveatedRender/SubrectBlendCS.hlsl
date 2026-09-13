// Blends DLSS subrect output back onto the stretched background in kMAIN.
// Replaces the hard CopySubresourceRegion with a feathered transition at the
// subrect boundary.  Three blend modes:
//   0 = Feather      – smoothstep alpha ramp over FeatherWidth pixels (inside)
//   1 = Dither       – noise-perturbed gradient in feather band (DitherStrength)
//   2 = OuterFeather – subrect stays full DLSS to its edge; a band OUTSIDE the
//                      subrect fades from the clamped DLSS edge color to the
//                      stretched background over OuterWidth pixels.  C0-continuous
//                      at the boundary (alpha=1 on both sides of the edge).

cbuffer BlendCB : register(b0)
{
	uint DstOffsetX;       // SBS destination X for this eye (0 or eyeWidthOut)
	uint DstOffsetY;       // SBS destination Y (usually 0, non-zero if subrect offset)
	uint SubWidth;         // DLSS output width  (subrect)
	uint SubHeight;        // DLSS output height (subrect)
	uint BlendMode;        // 0 = Feather, 1 = Dither, 2 = OuterFeather
	float FeatherWidth;    // Feather band in pixels (default ~64)
	uint FrameIndex;       // For dither noise animation
	uint SrcOffsetX;       // Source X offset (0 for most modes, non-zero for Extreme strip)
	float DitherStrength;  // 0 = pure smooth gradient, 1 = natural noise, 2 = aggressive dither
	float Roundness;       // 0 = rectangular boundary, 1 = elliptical
	uint DstLimitMinX;     // Eye region clamp (SBS): outer band must not spill into the other eye
	uint DstLimitMaxX;     // Exclusive upper bound; DstLimitMaxX==0 = no limit
	uint DstLimitMinY;     // Vertical clamp (subrect near the eye's top/bottom edge)
	uint DstLimitMaxY;     // Exclusive upper bound; DstLimitMaxY==0 = no limit
	float OuterWidth;      // Outer feather band width in pixels (mode 2)
	uint BandX;            // Dispatch padding in X (outer band size)
	uint BandY;            // Dispatch padding in Y
	float _pad0;
}

Texture2D<float4> SrcTex : register(t0);    // DLSS subrect output
RWTexture2D<float4> DstTex : register(u0);  // kMAIN (already has stretched background)

// Simple hash-based blue noise (no texture needed, near zero cost)
float BlueNoise(uint2 pos, uint frame)
{
	// Interleaved gradient noise (Jimenez 2014) — good spatial distribution
	float x = float(pos.x) + 5.588238 * float(frame);
	float y = float(pos.y) + 5.588238 * float(frame);
	return frac(52.9829189 * frac(0.06711056 * x + 0.00583715 * y));
}

// Signed distance to the subrect edge in pixels: positive inside, 0 at the
// boundary, negative outside.  Rectangular: max over per-axis signed distances.
// Roundness>0 morphs toward an ellipse (superellipse, same field as the
// interior modes so the boundary shape is identical on both sides).
float EdgeDistance(float2 pos)
{
	if (Roundness > 0.0) {
		float2 extent = float2(SubWidth, SubHeight) * 0.5;
		float2 scaled = abs(pos + 0.5 - extent) / extent;
		float shape = lerp(max(scaled.x, scaled.y), length(scaled), saturate(Roundness));
		// shape: 0 at center, 1 at edge midpoints, >1 past corners
		return (1.0 - shape) * min(extent.x, extent.y);
	}
	float extentX = SubWidth * 0.5;
	float extentY = SubHeight * 0.5;
	float dx = extentX - abs(pos.x + 0.5 - extentX);
	float dy = extentY - abs(pos.y + 0.5 - extentY);
	return min(dx, dy);
}

[numthreads(8, 8, 1)] void main(uint3 tid : SV_DispatchThreadID) {
	// Dispatch domain: interior (SubWidth x SubHeight) plus an OuterWidth band
	// on each side, all relative to an origin shifted by the band size.
	int2 base = (int2)tid.xy - int2(BandX, BandY);
	bool inside = base.x >= 0 && base.x < (int)SubWidth && base.y >= 0 && base.y < (int)SubHeight;

	int2 srcPos = int2(base.x + (int)SrcOffsetX, base.y);
	int2 dstPos = int2(base.x + (int)DstOffsetX, base.y + (int)DstOffsetY);

	if (BlendMode == 2) {
		// ── Outer feather ──
		// Eye-region clamp: the outer band must never write into the other
		// eye's half of the SBS target (binocular garbage) or outside the
		// target entirely. A zero Max means "no limit" on that axis.
		if ((DstLimitMaxX != 0 && (dstPos.x < (int)DstLimitMinX || dstPos.x >= (int)DstLimitMaxX)) ||
			(DstLimitMaxY != 0 && (dstPos.y < (int)DstLimitMinY || dstPos.y >= (int)DstLimitMaxY)))
			return;
		if (dstPos.x < 0 || dstPos.y < 0)
			return;

		if (inside) {
			// Interior: pure DLSS all the way to the edge — keeps the upscale
			// sharp and makes the boundary C0-continuous with the outer band.
			DstTex[uint2(dstPos)] = SrcTex.Load(int3(srcPos, 0));
			return;
		}

		// Outside the subrect: fade from the clamped DLSS edge color to the
		// background over the band. Clamp-sampling repeats the edge texel —
		// acceptable because the background under this band is the stretched
		// version of the same content, so edge colors nearly match.
		float dist = -EdgeDistance(float2(base));  // positive outside
		float alpha = 1.0 - smoothstep(0.0, OuterWidth, dist);
		// Full DLSS at the edge (alpha=1) decaying to pure background.
		if (alpha <= 0.0)
			return;

		// Clamp source coords to the subrect (edge-extend).
		int2 clamped = clamp(srcPos, int2((int)SrcOffsetX, 0),
			int2((int)(SrcOffsetX + SubWidth) - 1, (int)SubHeight - 1));
		float4 dlssEdge = SrcTex.Load(int3(clamped, 0));
		float4 bg = DstTex[uint2(dstPos)];
		DstTex[uint2(dstPos)] = lerp(bg, dlssEdge, alpha);
		return;
	}

	// ── Interior modes (Feather / Dither): unchanged domain ──
	if (!inside)
		return;

	uint2 srcU = uint2(srcPos);
	uint2 dstU = uint2(dstPos);

	float4 dlss = SrcTex.Load(int3(srcU, 0));

	// Distance from nearest edge of the SUBRECT in subrect-local pixel space.
	//
	// Bot-flagged Major bug (CodeRabbit + Copilot on the original PR): the
	// previous implementation used `srcPos.x` for distL/distR. In Extreme
	// strip mode, SrcOffsetX != 0 (each eye reads its half of a horizontally-
	// concatenated SBS strip), so srcPos.x = tid.x + SrcOffsetX could exceed
	// SubWidth, making distR negative and breaking the feather band entirely
	// — the strip would never blend correctly with the background. Use the
	// dispatch-local coordinates so distances are in [0, SubWidth-1] regardless
	// of the source-side offset.
	float distL = (float)base.x;
	float distR = (float)(SubWidth - 1 - base.x);
	float distT = (float)base.y;
	float distB = (float)(SubHeight - 1 - base.y);

	float edgeDist;
	if (Roundness > 0.0) {
		edgeDist = max(EdgeDistance(float2(base)), 0.0);
	} else {
		edgeDist = min(min(distL, distR), min(distT, distB));
	}

	if (edgeDist >= FeatherWidth) {
		// Interior: pure DLSS (fast path, skips background read)
		DstTex[dstPos] = dlss;
		return;
	}

	// We're in the feather band — need background
	float4 bg = DstTex[dstPos];

	if (BlendMode == 1) {
		// Dither: noise-perturbed continuous gradient
		// Noise shifts the blend threshold per-pixel → natural irregular boundary
		float t = edgeDist / FeatherWidth;  // 0 at edge, 1 at band end
		float noise = BlueNoise(srcPos, FrameIndex);
		float alpha = saturate(t + (noise - 0.5) * DitherStrength);
		DstTex[dstPos] = lerp(bg, dlss, alpha);
	} else {
		// Feather (default): smooth alpha ramp
		float alpha = smoothstep(0.0, FeatherWidth, edgeDist);
		DstTex[dstPos] = lerp(bg, dlss, alpha);
	}
}
