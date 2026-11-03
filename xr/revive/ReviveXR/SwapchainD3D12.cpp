#include "SwapchainD3D12.h"
#include "Common.h"
#include "Session.h"

#define XR_USE_GRAPHICS_API_D3D12
#include <d3d12.h>
#include <openxr/openxr_platform.h>

#include "../../src/echoxr_diag.h"
#include "../../src/echoxr_policy.h"

void EchoXR_Log(const char* fmt, ...);

static_assert(echoxr::kGameColorState == (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) &&
	echoxr::kGameDepthState == (D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
	D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), "echoxr_policy.h image states");

// ECHOXR_D3D12_THROTTLE=0 turns the wait before a release off (to compare); read once.
static bool ThrottleOn()
{
	static const bool on = [] {
		char text[8] = {};
		const DWORD n = GetEnvironmentVariableA("ECHOXR_D3D12_THROTTLE", text, sizeof(text));
		const bool off = n && n < sizeof(text) && (!_stricmp(text, "0") || !_stricmp(text, "off") ||
			!_stricmp(text, "false") || !_stricmp(text, "no"));
		if (off)
			EchoXR_Log("ECHOXR_D3D12_THROTTLE=%s: a swapchain release doesn't wait for the GPU (as up to 0.6.3)", text);
		return !off;
	}();
	return on;
}

ovrTextureSwapChainD3D12::ovrTextureSwapChainD3D12(struct ID3D12CommandQueue* queue)
	: m_Queue(queue)
	, m_Device()
	, m_Allocator()
	, m_AcquireBarriers()
	, m_ReleaseBarriers()
	, m_Fence()
	, m_Event()
	, m_Signaled(0)
	, m_LongWaits(0)
{
	if (m_Queue)
		m_Queue->AddRef();

	if (m_Queue && SUCCEEDED(m_Queue->GetDevice(IID_PPV_ARGS(&m_Device))))
	{
		m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_Allocator));
		if (ThrottleOn() && SUCCEEDED(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence))))
		{
			m_Fence->SetName(L"EchoXR: swapchain release fence");
			m_Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		}
	}
}

ovrTextureSwapChainD3D12::~ovrTextureSwapChainD3D12()
{
	for (uint32_t i = 0; i < Length; i++)
	{
		m_AcquireBarriers[i]->Release();
		m_ReleaseBarriers[i]->Release();
	}
	delete[] m_AcquireBarriers;
	delete[] m_ReleaseBarriers;

	if (m_Allocator)
		m_Allocator->Release();

	if (m_Fence)
		m_Fence->Release();
	if (m_Event)
		CloseHandle(m_Event);

	if (m_Device)
		m_Device->Release();

	if (m_Queue)
		m_Queue->Release();
}

// EchoXR: SteamVR copies the image at xrReleaseSwapchainImage, with command lists on the game's
// queue and two command allocators a swapchain that it takes in turn, resetting the one it used
// the release before last. Nothing waited for that work: when Echo VR's queue fell behind (a
// burst of cosmetics uploading) SteamVR reset an allocator the GPU still had to run, the D3D12
// debug layer's error #552, and the GPU could hang (DXGI_ERROR_DEVICE_HUNG). So a release waits
// until the GPU has done everything queued before the previous release began, which leaves at
// most about two frames queued. A removed device completes every fence, so this never waits on
// one; after 5 s it logs and goes on as before.
void ovrTextureSwapChainD3D12::WaitForEarlierRelease()
{
	if (!m_Fence || !m_Event)
		return;
	const UINT64 want = m_Signaled;
	if (want && m_Fence->GetCompletedValue() < want && SUCCEEDED(m_Fence->SetEventOnCompletion(want, m_Event)))
	{
		const ULONGLONG start = GetTickCount64();
		const DWORD waited = WaitForSingleObject(m_Event, 5000);
		const ULONGLONG ms = GetTickCount64() - start;
		if (waited != WAIT_OBJECT_0)
			EchoXR_Log("A swapchain release waited 5 s for the GPU to finish the frame before last and went on without it "
				"(SteamVR may now reuse command memory the GPU still needs)");
		else if (ms >= 50 && EchoXR_LogNth(++m_LongWaits))
			EchoXR_Log("A swapchain release waited %llu ms for the GPU to finish the frame before last (%llu such waits so far)",
				(unsigned long long)ms, (unsigned long long)m_LongWaits);
	}
	if (SUCCEEDED(m_Queue->Signal(m_Fence, want + 1)))
		m_Signaled = want + 1;
}

ovrResult ovrTextureSwapChainD3D12::Commit(ovrSession session)
{
	WaitForEarlierRelease();
	m_Queue->ExecuteCommandLists(1, (ID3D12CommandList**)&m_ReleaseBarriers[CurrentIndex]);
	ovrResult result = ovrTextureSwapChainData::Commit(session);
	m_Queue->ExecuteCommandLists(1, (ID3D12CommandList**)&m_AcquireBarriers[CurrentIndex]);
	return result;
}

ovrResult ovrTextureSwapChainD3D12::InitBarriers()
{
	m_AcquireBarriers = new ID3D12GraphicsCommandList*[Length];
	m_ReleaseBarriers = new ID3D12GraphicsCommandList*[Length];

	for (uint32_t i = 0; i < Length; i++)
	{
		XrSwapchainImageD3D12KHR image = ((XrSwapchainImageD3D12KHR*)Images)[i];

		D3D12_RESOURCE_STATES target = IsDepthFormat(Desc.Format) ?
			D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
		// EchoXR: the state Echo VR leaves the image in and expects it back in (its own barriers;
		// Revive assumed PIXEL_SHADER_RESOURCE, which the debug layer reported as error #527).
		const D3D12_RESOURCE_STATES game = (D3D12_RESOURCE_STATES)echoxr::GameImageState(IsDepthFormat(Desc.Format));

		CHK_HR(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_Allocator,
			nullptr, IID_PPV_ARGS(&m_AcquireBarriers[i])));

		D3D12_RESOURCE_BARRIER barrier;
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource = image.texture;
		barrier.Transition.StateBefore = target;
		barrier.Transition.StateAfter = game;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		m_AcquireBarriers[i]->SetName(L"EchoXR: swapchain image to the game");   // named in DRED's log
		m_AcquireBarriers[i]->ResourceBarrier(1, &barrier);
		m_AcquireBarriers[i]->Close();

		CHK_HR(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_Allocator,
			nullptr, IID_PPV_ARGS(&m_ReleaseBarriers[i])));

		barrier.Transition.StateBefore = game;
		barrier.Transition.StateAfter = target;
		m_ReleaseBarriers[i]->SetName(L"EchoXR: swapchain image to the runtime");
		m_ReleaseBarriers[i]->ResourceBarrier(1, &barrier);
		m_ReleaseBarriers[i]->Close();
	}

	m_Queue->ExecuteCommandLists(1, (ID3D12CommandList**)&m_AcquireBarriers[CurrentIndex]);

	return ovrSuccess;
}

ovrResult ovrTextureSwapChainD3D12::Create(ovrSession session, ID3D12CommandQueue* queue, const ovrTextureSwapChainDesc* desc, ovrTextureSwapChain* out_TextureSwapChain)
{
	// Do some format compatibility conversions before creating the swapchain
	DXGI_FORMAT format = NegotiateFormat(session, TextureFormatToDXGIFormat(desc->Format));
	assert(session->SupportsFormat(format));

	ovrTextureSwapChainD3D12* chain = new ovrTextureSwapChainD3D12(queue);
	CHK_OVR(chain->Init(session->Session, desc, format));
	CHK_OVR(chain->EnumerateImages<XrSwapchainImageD3D12KHR>(XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR));

	// If the app doesn't expect a typeless texture we need to attach the fully qualified format to each texture.
	// Depth formats are always considered typeless in the Oculus SDK, so no need to hook those.
	if (!(desc->MiscFlags & ovrTextureMisc_DX_Typeless) && !IsDepthFormat(desc->Format))
	{
		// Create the SRVs and RTVs with the real format that the application expects
		D3D12_SHADER_RESOURCE_VIEW_DESC srv = { format, (D3D12_SRV_DIMENSION)DescToViewDimension(&chain->Desc), D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING };
		D3D12_RENDER_TARGET_VIEW_DESC rtv = { format, (D3D12_RTV_DIMENSION)DescToViewDimension(&chain->Desc) };
		srv.Texture2D.MipLevels = chain->Desc.MipLevels;
		if (chain->Desc.ArraySize > 1)
		{
			srv.Texture2DArray.ArraySize = chain->Desc.ArraySize;
			rtv.Texture2DArray.ArraySize = chain->Desc.ArraySize;
		}

		for (uint32_t i = 0; i < chain->Length; i++)
		{
			XrSwapchainImageD3D12KHR image = ((XrSwapchainImageD3D12KHR*)chain->Images)[i];

			HRESULT hr = image.texture->SetPrivateData(RXR_SRV_DESC, sizeof(D3D12_SHADER_RESOURCE_VIEW_DESC), &srv);
			if (SUCCEEDED(hr))
				hr = image.texture->SetPrivateData(RXR_RTV_DESC, sizeof(D3D12_RENDER_TARGET_VIEW_DESC), &rtv);
			if (FAILED(hr))
				return ovrError_RuntimeException;
		}
	}

	CHK_HR(chain->InitBarriers());

	*out_TextureSwapChain = chain;
	return ovrSuccess;
}
