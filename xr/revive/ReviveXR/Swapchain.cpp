#include "Swapchain.h"
#include "Common.h"
#include "Session.h"
#include "../../src/echoxr_policy.h"

#include <openxr/openxr.h>
#include <dxgiformat.h>
#include <d3dcommon.h>

#include "../../src/echoxr_diag.h"
#include "../../src/echoxr_settings.h"

echoxr::ControllerSettings EchoXR_Controls();

// EchoXR: waits until the swapchain's newly acquired image may be drawn into. Revive waited
// one frame and went on when that ran out (XR_TIMEOUT_EXPIRED is a success code), so on a slow
// frame, such as Echo VR loading the lobby, the game drew into an image the compositor could
// still be reading, and the next release followed a wait that never succeeded. SteamVR's D3D12
// path can then leave the GPU waiting for good (DXGI_ERROR_DEVICE_HUNG). SwapchainWait = frame
// in EchoXR.txt brings Revive's way back, to compare. After 5 s it gives up with a log line and
// goes on as Revive did, rather than hang the game.
static XrResult WaitForImage(XrSwapchain swapchain, XrDuration firstTimeout)
{
	EchoXR_Call call("xrWaitSwapchainImage");
	const echoxr::SwapchainWait policy = EchoXR_Controls().swapchainWait;
	const LONGLONG start = GetTickCount64();
	XrSwapchainImageWaitInfo waitInfo = XR_TYPE(SWAPCHAIN_IMAGE_WAIT_INFO);
	waitInfo.timeout = firstTimeout > 1000000 ? firstTimeout : 1000000;   // at least 1 ms a try
	for (;;)
	{
		const XrResult result = xrWaitSwapchainImage(swapchain, &waitInfo);
		if (XR_FAILED(result))
			return result;
		const int64_t waitedNs = (int64_t)(GetTickCount64() - start) * 1000000;
		switch (echoxr::AfterSwapchainWait(result, policy, waitedNs))
		{
		case echoxr::WaitNext::Done:
			if (result == XR_TIMEOUT_EXPIRED)
				EchoXR_SwapchainWaitTimedOut(waitedNs, false);
			return result;
		case echoxr::WaitNext::GiveUp:
			EchoXR_SwapchainWaitTimedOut(waitedNs, true);
			return XR_TIMEOUT_EXPIRED;
		case echoxr::WaitNext::Again:
			break;
		}
	}
}

ovrResult ovrTextureSwapChainData::Commit(ovrSession session)
{
	XrSwapchainImageReleaseInfo releaseInfo = XR_TYPE(SWAPCHAIN_IMAGE_RELEASE_INFO);
	CHK_XR(xrReleaseSwapchainImage(Swapchain, &releaseInfo));

	if (!Desc.StaticImage)
	{
		XrSwapchainImageAcquireInfo acquireInfo = XR_TYPE(SWAPCHAIN_IMAGE_ACQUIRE_INFO);
		CHK_XR(xrAcquireSwapchainImage(Swapchain, &acquireInfo, &CurrentIndex));

		// TODO: We could move this wait to the end of EndFrame so we submit earlier
		CHK_XR(WaitForImage(Swapchain, (*session->CurrentFrame).predictedDisplayPeriod));
	}
	return ovrSuccess;
}

ovrResult ovrTextureSwapChainData::Init(XrSession session, const ovrTextureSwapChainDesc* desc, int64_t format)
{
	Desc = *desc;

	XrSwapchainCreateInfo createInfo = XR_TYPE(SWAPCHAIN_CREATE_INFO);
	if (desc->MiscFlags & ovrTextureMisc_ProtectedContent)
		createInfo.createFlags |= XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT;

	if (desc->StaticImage)
		createInfo.createFlags |= XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;

	// All Oculus swapchains allow sampling
	createInfo.usageFlags |= XR_SWAPCHAIN_USAGE_SAMPLED_BIT;

	// All Oculus swapchains allow transfers
	createInfo.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

	if (desc->BindFlags & ovrTextureBind_DX_RenderTarget)
		createInfo.usageFlags |= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;

	if (desc->BindFlags & ovrTextureBind_DX_UnorderedAccess)
		createInfo.usageFlags |= XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT;

	if (desc->BindFlags & ovrTextureBind_DX_DepthStencil)
		createInfo.usageFlags |= XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

	if (desc->MiscFlags & ovrTextureMisc_DX_Typeless)
		createInfo.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;

	createInfo.format = format;
	createInfo.sampleCount = desc->SampleCount;
	createInfo.width = desc->Width;
	createInfo.height = desc->Height;
	createInfo.faceCount = desc->Type == ovrTexture_Cube ? desc->ArraySize : 1;
	createInfo.arraySize = desc->Type == ovrTexture_Cube ? 1 : desc->ArraySize;
	createInfo.mipCount = desc->MipLevels;
	CHK_XR(xrCreateSwapchain(session, &createInfo, &Swapchain));

	// EchoXR: wineopenxr (D3D12) learns a swapchain's image count only when they're first
	// enumerated, and refuses an acquire before that (XR_ERROR_CALL_ORDER_INVALID). Native
	// runtimes don't mind; asking for the count first is valid everywhere.
	uint32_t imageCount = 0;
	CHK_XR(xrEnumerateSwapchainImages(Swapchain, 0, &imageCount, nullptr));
	if (EchoXR_Max())
		EchoXR_Log("Swapchain made: %ux%u, format %lld, %u sample(s), %u mip(s), %u layer(s), %u face(s), %u image(s)%s",
			createInfo.width, createInfo.height, (long long)createInfo.format, createInfo.sampleCount, createInfo.mipCount,
			createInfo.arraySize, createInfo.faceCount, imageCount, desc->StaticImage ? ", static" : "");

	XrSwapchainImageAcquireInfo acqInfo = XR_TYPE(SWAPCHAIN_IMAGE_ACQUIRE_INFO);
	CHK_XR(xrAcquireSwapchainImage(Swapchain, &acqInfo, &CurrentIndex));

	CHK_XR(WaitForImage(Swapchain, XR_NO_DURATION));   // EchoXR: was one look, then on
	return ovrSuccess;
}

bool ovrTextureSwapChainData::IsDepthFormat(ovrTextureFormat format)
{
	switch (format)
	{
	case OVR_FORMAT_D16_UNORM:            return true;
	case OVR_FORMAT_D24_UNORM_S8_UINT:    return true;
	case OVR_FORMAT_D32_FLOAT:            return true;
	case OVR_FORMAT_D32_FLOAT_S8X24_UINT: return true;
	default: return false;
	}
}

DXGI_FORMAT ovrTextureSwapChainData::TextureFormatToDXGIFormat(ovrTextureFormat format)
{
	// OpenXR always uses typeless formats
	switch (format)
	{
	case OVR_FORMAT_UNKNOWN:              return DXGI_FORMAT_UNKNOWN;
	case OVR_FORMAT_B5G6R5_UNORM:         return DXGI_FORMAT_B5G6R5_UNORM;
	case OVR_FORMAT_B5G5R5A1_UNORM:       return DXGI_FORMAT_B5G5R5A1_UNORM;
	case OVR_FORMAT_B4G4R4A4_UNORM:       return DXGI_FORMAT_B4G4R4A4_UNORM;
	case OVR_FORMAT_R8G8B8A8_UNORM:       return DXGI_FORMAT_R8G8B8A8_UNORM;
	case OVR_FORMAT_R8G8B8A8_UNORM_SRGB:  return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	case OVR_FORMAT_B8G8R8A8_UNORM:       return DXGI_FORMAT_B8G8R8A8_UNORM;
	case OVR_FORMAT_B8G8R8A8_UNORM_SRGB:  return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	case OVR_FORMAT_B8G8R8X8_UNORM:       return DXGI_FORMAT_B8G8R8X8_UNORM;
	case OVR_FORMAT_B8G8R8X8_UNORM_SRGB:  return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
	case OVR_FORMAT_R16G16B16A16_FLOAT:   return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case OVR_FORMAT_R11G11B10_FLOAT:      return DXGI_FORMAT_R11G11B10_FLOAT;

		// Depth formats
	case OVR_FORMAT_D16_UNORM:            return DXGI_FORMAT_D16_UNORM;
	case OVR_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_D24_UNORM_S8_UINT;
	case OVR_FORMAT_D32_FLOAT:            return DXGI_FORMAT_D32_FLOAT;
	case OVR_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;

		// Added in 1.5 compressed formats can be used for static layers
	case OVR_FORMAT_BC1_UNORM:            return DXGI_FORMAT_BC1_UNORM;
	case OVR_FORMAT_BC1_UNORM_SRGB:       return DXGI_FORMAT_BC1_UNORM;
	case OVR_FORMAT_BC2_UNORM:            return DXGI_FORMAT_BC2_UNORM;
	case OVR_FORMAT_BC2_UNORM_SRGB:       return DXGI_FORMAT_BC2_UNORM;
	case OVR_FORMAT_BC3_UNORM:            return DXGI_FORMAT_BC3_UNORM;
	case OVR_FORMAT_BC3_UNORM_SRGB:       return DXGI_FORMAT_BC3_UNORM;
	case OVR_FORMAT_BC6H_UF16:            return DXGI_FORMAT_BC6H_UF16;
	case OVR_FORMAT_BC6H_SF16:            return DXGI_FORMAT_BC6H_SF16;
	case OVR_FORMAT_BC7_UNORM:            return DXGI_FORMAT_BC7_UNORM;
	case OVR_FORMAT_BC7_UNORM_SRGB:       return DXGI_FORMAT_BC7_UNORM;

	default: return DXGI_FORMAT_UNKNOWN;
	}
}

D3D_SRV_DIMENSION ovrTextureSwapChainData::DescToViewDimension(const ovrTextureSwapChainDesc* desc)
{
	if (desc->ArraySize > 1)
	{
		if (desc->SampleCount > 1)
			return D3D_SRV_DIMENSION_TEXTURE2DMSARRAY;
		else
			return D3D_SRV_DIMENSION_TEXTURE2DARRAY;
	}
	else
	{
		if (desc->SampleCount > 1)
			return D3D_SRV_DIMENSION_TEXTURE2DMS;
		else
			return D3D_SRV_DIMENSION_TEXTURE2D;
	}
}

DXGI_FORMAT ovrTextureSwapChainData::NegotiateFormat(ovrSession session, DXGI_FORMAT format)
{
	DXGI_FORMAT chosen = NegotiateFormatQuiet(session, format);
	if (chosen != format)
		EchoXR_Log("Swapchain format %d isn't offered by the runtime, using %d", (int)format, (int)chosen);
	return chosen;
}

DXGI_FORMAT ovrTextureSwapChainData::NegotiateFormatQuiet(ovrSession session, DXGI_FORMAT format)
{
	if (session->SupportsFormat(format))
		return format;

	// EchoXR: D24S8 isn't there through Proton on AMD GPUs; D32S8 keeps the stencil.
	const int64_t depth = echoxr::NegotiateDepthFormat(format, false,
		session->SupportsFormat(DXGI_FORMAT_D32_FLOAT_S8X24_UINT));
	if (depth != format)
		return (DXGI_FORMAT)depth;

	// Upgrade R11G11B10F to RGBA16F if it's available
	if (format == DXGI_FORMAT_R11G11B10_FLOAT && session->SupportsFormat(DXGI_FORMAT_R16G16B16A16_FLOAT))
			return DXGI_FORMAT_R16G16B16A16_FLOAT;

	// If RGBA16F is not available, then attempt downgrading it to an 8-bit linear format
	if (format == DXGI_FORMAT_R11G11B10_FLOAT || format == DXGI_FORMAT_R16G16B16A16_FLOAT)
		return DXGI_FORMAT_R8G8B8A8_UNORM;

	// No runtime supports 8-bit formats without alpha, but easy to convert to one with alpha
	if (format == DXGI_FORMAT_B8G8R8X8_UNORM)
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	else if (format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB)
		return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

	return format;
}
