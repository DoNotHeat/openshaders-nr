#include "Integration.h"

#include "Renderer.h"
#include "Deferred.h"
#include "Features/HDRDisplay.h"
#include "Features/Upscaling.h"
#include "Features/Upscaling/FoveatedRender/Bridge.h"
#include "Features/Upscaling/FoveatedRender/Core.h"
#include "Globals.h"
#include "GpuPass.h"

#include <array>
#include <cmath>
#include <cstddef>

namespace NeuralRendering
{
	namespace
	{
		eastl::unique_ptr<Texture2D> color[2];
		std::uint32_t colorWidth = 0;
		std::uint32_t colorHeight = 0;
		DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
		std::uint32_t lastAppliedFrame = UINT32_MAX;
		bool writebackLogged = false;
		bool flatRouteWasActive = false;
		bool flatFrameGenerationBlockLogged = false;
		bool flatHdrBlockLogged = false;

		// Skin-mask composite state: a pristine pre-evaluate copy of kTOTAL, a
		// composite target, the composite shader + params, and the per-draw range
		// skip throttle. All sized/lazy-created on first use.
		eastl::unique_ptr<Texture2D> originalColor;
		eastl::unique_ptr<Texture2D> compositeColor;
		winrt::com_ptr<ID3D11ComputeShader> skinMaskCompositeCS;
		eastl::unique_ptr<ConstantBuffer> skinMaskCB;
		winrt::com_ptr<ID3D11SamplerState> skinMaskSampler;
		std::uint32_t skipLoggedFrame = 0;
		bool skinMaskLogged = false;

		// Layout must mirror SkinMaskParams in SkinMaskCompositeCS.hlsl:
		// maskScale (offset 0), rampScale (offset 8), pad, debugVisualize (offset 16).
		// A mismatch here made rampScale read the debug flag -> weight = 0 everywhere
		// (invisible effect AND dead debug view).
		struct SkinMaskCB
		{
			float maskScaleX;
			float maskScaleY;
			float rampScale;
			float pad0;
			std::uint32_t debugVisualize;
			float pad1[3];
		};
		static_assert(offsetof(SkinMaskCB, debugVisualize) == 16, "SkinMaskCB layout must match SkinMaskParams cbuffer");
		static_assert(sizeof(SkinMaskCB) % 16 == 0);

		bool IsSkinMaskEnabled(const FoveatedRender& foveated)
		{
			return foveated.settings.neuralRenderingSkinMaskOnly;
		}

		struct PreScaleCB
		{
			float originX;
			float originY;
			float sizeX;
			float sizeY;
		};

		// Neural Quality < 100%: eval-sized (downsampled) color/depth/mvec textures,
		// lazily (re)created when the eval size changes.
		eastl::unique_ptr<Texture2D> preScaleColor[2];
		eastl::unique_ptr<Texture2D> preScaleDepth[2];
		eastl::unique_ptr<Texture2D> preScaleMvec[2];
		std::uint32_t preScaleW = 0;
		std::uint32_t preScaleH = 0;
		winrt::com_ptr<ID3D11ComputeShader> neuralPreScaleCS;
		eastl::unique_ptr<ConstantBuffer> preScaleCB;

		bool EnsurePreScaleResources(ID3D11Resource* source, std::uint32_t evalW, std::uint32_t evalH)
		{
			if (preScaleColor[0] && preScaleW == evalW && preScaleH == evalH)
				return true;
			for (std::uint32_t eye = 0; eye < 2; ++eye) {
				preScaleColor[eye] = Upscaling::CreateTextureFromSource(source, evalW, evalH, false, true, true,
					eye == 0 ? "NeuralRendering::PreScaleColorL" : "NeuralRendering::PreScaleColorR");
				preScaleDepth[eye] = Upscaling::CreateTextureFromSource(source, evalW, evalH, false, true, true,
					eye == 0 ? "NeuralRendering::PreScaleDepthL" : "NeuralRendering::PreScaleDepthR");
				preScaleMvec[eye] = Upscaling::CreateTextureFromSource(source, evalW, evalH, false, true, true,
					eye == 0 ? "NeuralRendering::PreScaleMvecL" : "NeuralRendering::PreScaleMvecR");
				if (!preScaleColor[eye] || !preScaleDepth[eye] || !preScaleMvec[eye])
					return false;
			}
			preScaleW = evalW;
			preScaleH = evalH;
			return true;
		}

		/// True when the DLSSNR pass should be skipped this frame: the character mask
		/// is on and either no character draw happened, or the nearest character is
		/// beyond the user's range (0 = range gating disabled).
		bool ShouldSkipForCharacterMask(const FoveatedRender& foveated, std::uint32_t frame)
		{
			if (!IsSkinMaskEnabled(foveated) || !globals::state)
				return false;
			if (!globals::state->sawCharacterThisFrame)
				return true;
			const float range = foveated.settings.neuralRenderingCharacterRange;
			if (range > 0.0f && globals::state->nearestCharacterDistance > range) {
				if (frame - skipLoggedFrame > 60) {
					logger::info("[DLSSNR] character-mask skip: nearest character {:.0f} beyond range {:.0f} (frame={})",
						globals::state->nearestCharacterDistance, range, frame);
					skipLoggedFrame = frame;
				}
				return true;
			}
			return false;
		}

		/// Face-mask gated write-back for one whole SBS frame. Reads the pristine
		/// pre-evaluate copy and the current (DLSSNR-evaluated) kTOTAL, and writes
		/// the mask-blended result into the composite target. Where the face mask
		/// (GBuffer Masks.y) is set, the frame blends toward the DLSSNR result;
		/// elsewhere the original pixels are kept. Outside the foveal subrects the
		/// neural input equals the original, so the blend is a no-op there
		/// regardless of the mask.
		bool CompositeWithSkinMask(ID3D11DeviceContext* context,
			ID3D11ShaderResourceView* originalSRV, ID3D11ShaderResourceView* neuralSRV,
			ID3D11UnorderedAccessView* dstUAV,
			float maskWidth, float maskHeight, std::uint32_t colorW, std::uint32_t colorH,
			bool debugVisualize)
		{
			auto& masks = globals::game::renderer->GetRuntimeData().renderTargets[MASKS];
			if (!masks.SRV || !originalSRV || !neuralSRV || !dstUAV)
				return false;

			if (!skinMaskCB)
				skinMaskCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<SkinMaskCB>(), "NeuralRendering::SkinMaskCB");

			SkinMaskCB cbData{};
			// Normalized render/display mapping; both SBS spaces share origin (0,0).
			// rampScale 2.0 maps Masks.y 0.5 (beast face) and 1.0 (human face) to full
			// weight, and spreads bilinear edge values (0..0.5) into a soft feather.
			cbData.maskScaleX = maskWidth / static_cast<float>(colorW);
			cbData.maskScaleY = maskHeight / static_cast<float>(colorH);
			cbData.rampScale = 2.0f;
			cbData.debugVisualize = debugVisualize ? 1u : 0u;
			skinMaskCB->Update(&cbData, sizeof(cbData));

			if (!skinMaskCompositeCS) {
				skinMaskCompositeCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
					L"Data\\Shaders\\Upscaling\\NeuralRendering\\SkinMaskCompositeCS.hlsl", {}, "cs_5_0")));
				Util::SetResourceName(skinMaskCompositeCS.get(), "NeuralRendering::SkinMaskCompositeCS");
				if (!skinMaskCompositeCS)
					return false;
			}

			CS_GPU_PASS("NeuralRendering::SkinMaskComposite");

			if (!skinMaskSampler) {
				D3D11_SAMPLER_DESC samplerDesc{};
				samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
				samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
				samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
				samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
				if (FAILED(globals::d3d::device->CreateSamplerState(&samplerDesc, skinMaskSampler.put())))
					return false;
				Util::SetResourceName(skinMaskSampler.get(), "NeuralRendering::SkinMaskSampler");
			}

			// The composite shader declares its own MaskSampler register; bind the
			// sampler at slot 0 (the shader's sampler space is separate in cslot terms).
			ID3D11ShaderResourceView* srvs[3] = { originalSRV, neuralSRV, masks.SRV };
			ID3D11UnorderedAccessView* uavs[1] = { dstUAV };
			ID3D11Buffer* cb = skinMaskCB->CB();
			ID3D11SamplerState* sampler = skinMaskSampler.get();

			context->CSSetConstantBuffers(0, 1, &cb);
			context->CSSetSamplers(0, 1, &sampler);
			context->CSSetShaderResources(0, 3, srvs);
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->CSSetShader(skinMaskCompositeCS.get(), nullptr, 0);
			context->Dispatch((colorW + 7) / 8, (colorH + 7) / 8, 1);

			ID3D11ShaderResourceView* nullSRVs[3]{};
			ID3D11UnorderedAccessView* nullUAVs[1]{};
			context->CSSetShaderResources(0, 3, nullSRVs);
			context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
			ID3D11Buffer* nullCB = nullptr;
			context->CSSetConstantBuffers(0, 1, &nullCB);

			if (!skinMaskLogged) {
				logger::info("[DLSSNR] skin-mask composite active mask={}x{} region={}x{} debug={}",
					static_cast<int>(maskWidth), static_cast<int>(maskHeight), colorW, colorH, debugVisualize ? 1 : 0);
				skinMaskLogged = true;
			}
			return true;
		}

		ID3D11Texture2D* ResolveRenderTargetTexture(
			const RE::BSGraphics::RenderTargetData& target,
			winrt::com_ptr<ID3D11Texture2D>& holder)
		{
			if (target.texture)
				return target.texture;
			auto resolveView = [&](ID3D11View* view) -> ID3D11Texture2D* {
				if (!view)
					return nullptr;
				winrt::com_ptr<ID3D11Resource> resource;
				view->GetResource(resource.put());
				if (!resource || FAILED(resource->QueryInterface(holder.put())))
					return nullptr;
				return holder.get();
			};
			if (auto* texture = resolveView(target.SRV))
				return texture;
			return resolveView(target.RTV);
		}

		bool EnsureColorResources(ID3D11Resource* source, std::uint32_t width, std::uint32_t height)
		{
			winrt::com_ptr<ID3D11Texture2D> sourceTexture;
			if (!source || FAILED(source->QueryInterface(sourceTexture.put())))
				return false;
			D3D11_TEXTURE2D_DESC sourceDesc{};
			sourceTexture->GetDesc(&sourceDesc);
			if (color[0] && colorWidth == width && colorHeight == height && colorFormat == sourceDesc.Format)
				return true;
			const std::uint32_t resourceCount = globals::game::isVR ? 2u : 1u;
			for (std::uint32_t eye = 0; eye < resourceCount; ++eye) {
				color[eye] = Upscaling::CreateTextureFromSource(source, width, height, false, true, true,
					eye == 0 ? "NeuralRendering::LdrColorLeft" : "NeuralRendering::LdrColorRight");
				if (!color[eye])
					return false;
			}
			if (!globals::game::isVR)
				color[1].reset();
			colorWidth = width;
			colorHeight = height;
			colorFormat = sourceDesc.Format;
			return true;
		}

		Tuning GetTuning(const FoveatedRender::Settings& settings, float intensityScale)
		{
			return {
				settings.neuralRenderingIntensity * intensityScale,
				settings.neuralRenderingLocalTone,
				settings.neuralRenderingLocalStructure,
				settings.neuralRenderingSkinStructure,
				settings.neuralRenderingStyle,
				settings.neuralRenderingAutoMask,
				settings.neuralRenderingUICorrection,
			};
		}

		bool ApplyFlatLdr(Upscaling& upscaling, FoveatedRender& foveated)
		{
			const bool frameGenerationConfigured = upscaling.IsFrameGenerationConfiguredForSession();
			const bool hdrConfigured = globals::features::hdrDisplay.loaded &&
				globals::features::hdrDisplay.settings.enableHDR;
			auto* renderer = globals::game::renderer;
			winrt::com_ptr<ID3D11Texture2D> framebufferHolder;
			ID3D11Texture2D* framebuffer = nullptr;
			if (renderer) {
				auto& target = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
				framebuffer = ResolveRenderTargetTexture(target, framebufferHolder);
			}
			const bool routeActive = upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS &&
				foveated.settings.neuralRenderingEnabled && !frameGenerationConfigured && !hdrConfigured;
			if (!routeActive) {
				if (foveated.settings.neuralRenderingEnabled && frameGenerationConfigured && !flatFrameGenerationBlockLogged) {
					logger::warn("[DLSSNR] Flat route blocked: disable Frame Generation and restart the game");
					flatFrameGenerationBlockLogged = true;
				}
				if (foveated.settings.neuralRenderingEnabled && hdrConfigured && !flatHdrBlockLogged) {
					logger::warn("[DLSSNR] Flat route blocked: HDR Display is not supported by the LDR integration");
					flatHdrBlockLogged = true;
				}
				if (flatRouteWasActive)
					Reset();
				return false;
			}
			flatRouteWasActive = true;

			const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;
			if (lastAppliedFrame == frame)
				return true;
			auto* context = globals::d3d::context;
			if (!renderer || !context || !globals::d3d::device || !upscaling.motionVectorCopyTexture)
				return false;

			auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			if (!framebuffer || !depth.texture || !depth.depthSRV || !upscaling.motionVectorCopyTexture->resource)
				return false;

			D3D11_TEXTURE2D_DESC totalDesc{};
			D3D11_TEXTURE2D_DESC motionDesc{};
			framebuffer->GetDesc(&totalDesc);
			upscaling.motionVectorCopyTexture->resource->GetDesc(&motionDesc);
			if (!EnsureColorResources(framebuffer, totalDesc.Width, totalDesc.Height))
				return false;

			CS_GPU_PASS("NeuralRendering::FlatLdrBeforeUI");
			ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView* savedDSV = nullptr;
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
			context->OMSetRenderTargets(0, nullptr, nullptr);
			context->CopyResource(color[0]->resource.get(), framebuffer);

			const bool succeeded = Renderer::Instance().Apply(globals::d3d::device, context, 0,
				color[0]->resource.get(), depth.texture, depth.depthSRV,
				upscaling.motionVectorCopyTexture->resource.get(), motionDesc.Width, motionDesc.Height,
				totalDesc.Width, totalDesc.Height, static_cast<float>(motionDesc.Width),
				static_cast<float>(motionDesc.Height), GetTuning(foveated.settings, 1.0f));
			if (succeeded) {
				context->CopyResource(framebuffer, color[0]->resource.get());
				lastAppliedFrame = frame;
				if (!writebackLogged) {
					logger::info("[DLSSNR] Flat LDR kFRAMEBUFFER output written before UI guides={}x{} color={}x{}",
						motionDesc.Width, motionDesc.Height, totalDesc.Width, totalDesc.Height);
					writebackLogged = true;
				}
			}

			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
			for (auto*& rtv : savedRTVs)
				if (rtv) rtv->Release();
			if (savedDSV) savedDSV->Release();
			return succeeded;
		}
	}

	bool ApplyFoveatedLdr()
	{
		auto& upscaling = globals::features::upscaling;
		auto& foveated = upscaling.foveatedRender;
		if (!globals::game::isVR)
			return ApplyFlatLdr(upscaling, foveated);
		if (!globals::game::isVR || !FoveatedRenderImpl::Bridge::IsRouteActive() ||
			upscaling.GetUpscaleMethod() != Upscaling::UpscaleMethod::kDLSS ||
			foveated.GetDlssMode() != FoveatedRender::DlssMode::kDefault ||
			!foveated.settings.neuralRenderingEnabled || upscaling.IsFrameGenerationActive())
			return false;

		const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;
		const std::uint32_t guideFrame = FoveatedRenderImpl::Core::neuralGuidesFrame;
		if (lastAppliedFrame == frame || (guideFrame != frame && !(frame > 0 && guideFrame == frame - 1)))
			return false;

		// Character-mask skip: with the mask on and no in-range character drawn this
		// frame, the DLSSNR result would be invisible everywhere — skip the evaluate
		// (the expensive part) entirely. The NGX feature keeps its temporal history
		// internally; skipping frames is safe because the next evaluated frame resumes
		// from that history.
		if (ShouldSkipForCharacterMask(foveated, frame)) {
			lastAppliedFrame = frame;
			return true;
		}

		// Gaze-following foveation moves the subrect between frames, and NR's
		// temporal history lags the moved window — evaluating at full strength
		// mid-move reads as breathing shadows / jittering surroundings. NR
		// stays ON every frame (on/off switching flickers far worse); its
		// Intensity eases toward zero while the subrect glides and recovers
		// immediately as the steps shrink, so the effect is back at full
		// strength roughly when the region reaches the new fixation point.
		static float nrIntensityScale = 1.0f;
		constexpr float kMoveFadePerFrame = 0.4f;    // toward 0 while moving
		constexpr float kSettleFadePerFrame = 0.35f; // back toward 1 when still
		const float targetScale = foveated.subrectMovedThisFrame ? 0.0f : 1.0f;
		const float fadeRate = (targetScale < nrIntensityScale) ? kMoveFadePerFrame : kSettleFadePerFrame;
		nrIntensityScale += (targetScale - nrIntensityScale) * fadeRate;
		if (nrIntensityScale < 0.01f)
			nrIntensityScale = 0.0f;
		if (nrIntensityScale > 0.99f)
			nrIntensityScale = 1.0f;

		auto* renderer = globals::game::renderer;
		auto* context = globals::d3d::context;
		if (!renderer || !context || !globals::d3d::device ||
			!FoveatedRenderImpl::Core::vrSubrectDepth[0] || !FoveatedRenderImpl::Core::vrSubrectDepth[1] ||
			!FoveatedRenderImpl::Core::vrSubrectMotionVectors[0] || !FoveatedRenderImpl::Core::vrSubrectMotionVectors[1])
			return false;
		auto& total = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTOTAL];
		if (!total.texture)
			return false;

		D3D11_TEXTURE2D_DESC totalDesc{};
		total.texture->GetDesc(&totalDesc);
		const auto& leftUV = foveated.subrectController.GetUV();
		const auto& rightUV = foveated.subrectController.GetRightEyeUV();
		if (leftUV.w != rightUV.w || leftUV.h != rightUV.h)
			return false;

		// Gaze-following foveation moves the subrect between frames, so the
		// depth/mvec guides cached by the DLSS route may be one frame stale —
		// cropped at the PREVIOUS subrect position while the color crop below
		// reads the CURRENT one. Re-crop the guides at the current position
		// right here instead of skipping the evaluation: a skip reads as a
		// visible NR flicker (stale output held for a frame) on every glide
		// step, while a re-crop keeps color and guides aligned every frame.
		// Guides live in the full SBS depth/mvec textures, so the crop boxes
		// mirror the DLSS route's per-eye boxes exactly.
		if (guideFrame != frame) {
			const std::uint32_t eyeWidthIn = totalDesc.Width / 2;
			const std::uint32_t eyeHeightIn = totalDesc.Height;
			const Util::Subrect::UVRegion* guideUVs[2]{ &leftUV, &rightUV };
			for (std::uint32_t eye = 0; eye < 2; ++eye) {
				const auto& uv = *guideUVs[eye];
				const std::uint32_t subInW = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(eyeWidthIn * uv.w));
				const std::uint32_t subInH = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(eyeHeightIn * uv.h));
				const std::uint32_t cropX = static_cast<std::uint32_t>(uv.x * eyeWidthIn);
				const std::uint32_t cropY = static_cast<std::uint32_t>(uv.y * eyeHeightIn);
				const std::uint32_t sbsX = (eye ? eyeWidthIn : 0) + cropX;
				const D3D11_BOX sbsCrop{ sbsX, cropY, 0, sbsX + subInW, cropY + subInH, 1 };
				context->CopySubresourceRegion(FoveatedRenderImpl::Core::vrSubrectDepth[eye]->resource.get(), 0, 0, 0, 0,
					renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture, 0, &sbsCrop);
				context->CopySubresourceRegion(FoveatedRenderImpl::Core::vrSubrectMotionVectors[eye]->resource.get(), 0, 0, 0, 0,
					upscaling.motionVectorCopyTexture ? upscaling.motionVectorCopyTexture->resource.get() : nullptr, 0, &sbsCrop);
			}
			FoveatedRenderImpl::Core::neuralGuidesFrame = frame;
		}
		const std::uint32_t eyeWidth = totalDesc.Width / 2;
		const std::uint32_t outWidth = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(eyeWidth * leftUV.w));
		const std::uint32_t outHeight = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(totalDesc.Height * leftUV.h));

		// Neural Quality: evaluate only a scaled portion of the foveal subrect. NGX
		// cost scales with the evaluated area, so 50% quality costs ~25% of the pass.
		// The WHOLE subrect is downsampled (color from kTOTAL, guides from the
		// subrect textures) into eval-sized textures — cropping the top-left corner
		// would feed NGX a zoomed fragment desynced from the guides (temporal
		// flicker). ApplyStereo then crops the eval-sized result back into kTOTAL.
		const std::uint32_t quality = std::clamp(foveated.settings.neuralRenderingQuality, 25u, 100u);
		const float qualityScale = static_cast<float>(quality) / 100.0f;
		const std::uint32_t evalWidth = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(outWidth * qualityScale));
		const std::uint32_t evalHeight = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(outHeight * qualityScale));
		const bool preScaleActive = evalWidth < outWidth || evalHeight < outHeight;

		const Util::Subrect::UVRegion* eyeUVs[2]{ &leftUV, &rightUV };

		CS_GPU_PASS("NeuralRendering::FoveatedLdrBeforeUI");
		ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* savedDSV = nullptr;
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
		context->OMSetRenderTargets(0, nullptr, nullptr);

		// Snapshot the pristine whole frame BEFORE ApplyStereo overwrites the subrects
		// (the skin-mask composite needs the pre-evaluate originals to blend against).
		const bool skinMaskActive = IsSkinMaskEnabled(foveated);
		if (skinMaskActive && !EnsureColorResources(total.texture, totalDesc.Width, totalDesc.Height)) {
			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
			for (auto*& rtv : savedRTVs)
				if (rtv) rtv->Release();
			if (savedDSV) savedDSV->Release();
			return false;
		}
		if (skinMaskActive) {
			if (!originalColor)
				originalColor = Upscaling::CreateTextureFromSource(total.texture, totalDesc.Width, totalDesc.Height, false, true, true,
					"NeuralRendering::SkinMaskOriginal");
			if (!compositeColor)
				compositeColor = Upscaling::CreateTextureFromSource(total.texture, totalDesc.Width, totalDesc.Height, false, true, true,
					"NeuralRendering::SkinMaskComposite");
			if (!originalColor || !compositeColor || !originalColor->srv || !compositeColor->uav) {
				context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
				for (auto*& rtv : savedRTVs)
					if (rtv) rtv->Release();
				if (savedDSV) savedDSV->Release();
				return false;
			}
			context->CopyResource(originalColor->resource.get(), total.texture);
		}

		// Neural Quality < 100%: downsample the WHOLE subrect (color from kTOTAL,
		// depth/mvec guides) into eval-sized textures. The pre-scale pass runs per
		// eye; ApplyStereo then crops from these eval-sized textures.
		if (preScaleActive) {			if (!EnsurePreScaleResources(total.texture, evalWidth, evalHeight)) {
				context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
				for (auto*& rtv : savedRTVs)
					if (rtv) rtv->Release();
				if (savedDSV) savedDSV->Release();
				return false;
			}
			CS_GPU_PASS("NeuralRendering::PreScale");
			if (!preScaleCB)
				preScaleCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<PreScaleCB>(), "NeuralRendering::PreScaleCB");
			if (!neuralPreScaleCS) {
				neuralPreScaleCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
					L"Data\\Shaders\\Upscaling\\NeuralRendering\\NeuralPreScaleCS.hlsl", {}, "cs_5_0")));
				Util::SetResourceName(neuralPreScaleCS.get(), "NeuralRendering::NeuralPreScaleCS");
			}
			if (!neuralPreScaleCS || !preScaleCB) {
				context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
				for (auto*& rtv : savedRTVs)
					if (rtv) rtv->Release();
				if (savedDSV) savedDSV->Release();
				return false;
			}
			for (std::uint32_t eye = 0; eye < 2; ++eye) {
				const auto& uv = *eyeUVs[eye];
				PreScaleCB cbData{};
				cbData.originX = (eye ? 0.5f : 0.0f) + static_cast<float>(eyeWidth * uv.x) / static_cast<float>(totalDesc.Width);
				cbData.originY = static_cast<float>(totalDesc.Height * uv.y) / static_cast<float>(totalDesc.Height);
				cbData.sizeX = static_cast<float>(outWidth) / static_cast<float>(totalDesc.Width);
				cbData.sizeY = static_cast<float>(outHeight) / static_cast<float>(totalDesc.Height);
				preScaleCB->Update(&cbData, sizeof(cbData));

				if (!skinMaskSampler) {
					D3D11_SAMPLER_DESC samplerDesc{};
					samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
					samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
					samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
					samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
					if (FAILED(globals::d3d::device->CreateSamplerState(&samplerDesc, skinMaskSampler.put()))) {
						context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
						for (auto*& rtv : savedRTVs)
							if (rtv) rtv->Release();
						if (savedDSV) savedDSV->Release();
						return false;
					}
					Util::SetResourceName(skinMaskSampler.get(), "NeuralRendering::SkinMaskSampler");
				}

				ID3D11ShaderResourceView* srvs[3] = {
					total.SRV,
					FoveatedRenderImpl::Core::vrSubrectDepth[eye]->srv.get(),
					FoveatedRenderImpl::Core::vrSubrectMotionVectors[eye]->srv.get(),
				};
				ID3D11UnorderedAccessView* uavs[3] = {
					preScaleColor[eye]->uav.get(),
					preScaleDepth[eye]->uav.get(),
					preScaleMvec[eye]->uav.get(),
				};
				ID3D11Buffer* cb = preScaleCB->CB();
				ID3D11SamplerState* sampler = skinMaskSampler.get();

				context->CSSetConstantBuffers(0, 1, &cb);
				context->CSSetSamplers(0, 1, &sampler);
				context->CSSetShaderResources(0, 3, srvs);
				context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
				context->CSSetShader(neuralPreScaleCS.get(), nullptr, 0);
				context->Dispatch((evalWidth + 7) / 8, (evalHeight + 7) / 8, 1);

				ID3D11ShaderResourceView* nullSRVs[3]{};
				ID3D11UnorderedAccessView* nullUAVs[3]{};
				context->CSSetShaderResources(0, 3, nullSRVs);
				context->CSSetUnorderedAccessViews(0, 3, nullUAVs, nullptr);
				context->CSSetShader(nullptr, nullptr, 0);
				ID3D11Buffer* nullCB = nullptr;
				context->CSSetConstantBuffers(0, 1, &nullCB);
			}
		}

		std::array<Renderer::StereoEyeInput, 2> inputs{};
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			const auto& uv = *eyeUVs[eye];
			const std::uint32_t x = (eye ? eyeWidth : 0) + static_cast<std::uint32_t>(eyeWidth * uv.x);
			const std::uint32_t y = static_cast<std::uint32_t>(totalDesc.Height * uv.y);
			// Diagnostic: NR read/write position on kTOTAL, correlated by
			// frame number with [FOVEATED-DIAG]. Per-eye statics: a shared
			// timer would starve eye 1.
			static std::chrono::steady_clock::time_point lastLog[2];
			auto now = std::chrono::steady_clock::now();
			if (now - lastLog[eye] > std::chrono::seconds(5)) {
				lastLog[eye] = now;
				logger::info("[DLSSNR-DIAG] NR writeback frame={} eye={} srcX={} srcY={} size={}x{} eyeWidth={} total={}x{} leftUV=({:.3f},{:.3f},{:.3f},{:.3f})",
					frame, eye, x, y, outWidth, outHeight, eyeWidth, totalDesc.Width, totalDesc.Height,
					leftUV.x, leftUV.y, leftUV.w, leftUV.h);
			}

			float motionScaleX = 1.0f;
			float motionScaleY = 1.0f;
			FoveatedRenderImpl::Bridge::ComputeMvecScale(eye, motionScaleX, motionScaleY);
			inputs[eye] = {
				.depth = FoveatedRenderImpl::Core::vrSubrectDepth[eye]->resource.get(),
				.depthSRV = FoveatedRenderImpl::Core::vrSubrectDepth[eye]->srv.get(),
				.motionVectors = FoveatedRenderImpl::Core::vrSubrectMotionVectors[eye]->resource.get(),
				.sourceX = x,
				.sourceY = y,
				.motionVectorScaleX = motionScaleX * FoveatedRenderImpl::Core::vrSubrectInW,
				.motionVectorScaleY = motionScaleY * FoveatedRenderImpl::Core::vrSubrectInH,
			};
		}
		// Point the evaluate at the eval-sized prescaled inputs. sourceX/Y stay at
		// the subrect origin: ApplyStereo crops evalW x evalH from the eval-sized
		// textures (a full copy) and writes the result back into kTOTAL there.
		if (preScaleActive) {
			for (std::uint32_t eye = 0; eye < 2; ++eye) {
				inputs[eye].depth = preScaleDepth[eye]->resource.get();
				inputs[eye].depthSRV = preScaleDepth[eye]->srv.get();
				inputs[eye].motionVectors = preScaleMvec[eye]->resource.get();
			}
		}
		const bool succeeded = Renderer::Instance().ApplyStereo(globals::d3d::device, context,
			total.texture, inputs, FoveatedRenderImpl::Core::vrSubrectInW, FoveatedRenderImpl::Core::vrSubrectInH,
			outWidth, outHeight, GetTuning(foveated.settings, nrIntensityScale));
		if (succeeded) {
			lastAppliedFrame = frame;
			if (!writebackLogged) {
				logger::info("[DLSSNR] LDR output written before UI composite size={}x{} batchedAsync=true", outWidth, outHeight);
				writebackLogged = true;
			}
		}

		if (succeeded && skinMaskActive) {
			// VR stereo, whole-frame composite. After ApplyStereo, kTOTAL holds the
			// DLSSNR output inside the foveal subrects and the original pixels outside
			// them. Composite the full frame: where the face mask is set, blend toward
			// the DLSSNR result; elsewhere keep the original. Outside the subrects the
			// neural input equals the original, so the blend is a no-op regardless of
			// the mask. Both SBS spaces share origin (0,0), so a single normalized
			// mapping covers both eyes.
			auto& masks = renderer->GetRuntimeData().renderTargets[MASKS];
			if (masks.SRV) {
				D3D11_TEXTURE2D_DESC masksDesc{};
				masks.texture->GetDesc(&masksDesc);
				if (CompositeWithSkinMask(context,
						originalColor->srv.get(), total.SRV, compositeColor->uav.get(),
						static_cast<float>(masksDesc.Width), static_cast<float>(masksDesc.Height),
						totalDesc.Width, totalDesc.Height,
						foveated.settings.neuralRenderingSkinMaskDebug)) {
					context->CopyResource(total.texture, compositeColor->resource.get());
				}
			} else {
				logger::warn("[DLSSNR] skin mask: MASKS SRV unavailable, DLSSNR output kept as-is");
			}
		}

		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
		for (auto*& rtv : savedRTVs)
			if (rtv) rtv->Release();
		if (savedDSV) savedDSV->Release();
		return succeeded;
	}

	void Reset()
	{
		Renderer::Instance().Reset();
		color[0].reset();
		color[1].reset();
		originalColor.reset();
		compositeColor.reset();
		for (auto& tex : preScaleColor) tex.reset();
		for (auto& tex : preScaleDepth) tex.reset();
		for (auto& tex : preScaleMvec) tex.reset();
		preScaleW = preScaleH = 0;
		neuralPreScaleCS = nullptr;
		preScaleCB.reset();
		skinMaskCompositeCS = nullptr;
		skinMaskCB.reset();
		colorWidth = colorHeight = 0;
		colorFormat = DXGI_FORMAT_UNKNOWN;
		lastAppliedFrame = UINT32_MAX;
		writebackLogged = false;
		flatRouteWasActive = false;
		flatFrameGenerationBlockLogged = false;
		flatHdrBlockLogged = false;
	}
}
