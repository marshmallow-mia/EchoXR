// What runtime.log says about a graphics device that stops responding (see echoxr_diag.h).
#include "echoxr_diag.h"
#include "echoxr_policy.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>

using Microsoft::WRL::ComPtr;

void EchoXR_Log(const char* fmt, ...);

namespace {

std::atomic<const char*> g_call{ nullptr };
std::atomic<uint64_t> g_slowCalls{ 0 };
std::atomic<uint64_t> g_timedOutWaits{ 0 };
std::atomic<uint64_t> g_gaveUpWaits{ 0 };

std::mutex g_watchLock;
ID3D12Device* g_device = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_removedEvent = nullptr;
HANDLE g_wait = nullptr;
PVOID g_breakpointHandler = nullptr;
std::atomic<bool> g_removalLogged{ false };

const char* OpName(D3D12_AUTO_BREADCRUMB_OP op)
{
	static const char* const names[] = {
		"SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced",
		"ExecuteIndirect", "Dispatch", "CopyBufferRegion", "CopyTextureRegion", "CopyResource",
		"CopyTiles", "ResolveSubresource", "ClearRenderTargetView", "ClearUnorderedAccessView",
		"ClearDepthStencilView", "ResourceBarrier", "ExecuteBundle", "Present", "ResolveQueryData",
		"BeginSubmission", "EndSubmission", "DecodeFrame", "ProcessFrames", "AtomicCopyBufferUint",
		"AtomicCopyBufferUint64", "ResolveSubresourceRegion", "WriteBufferImmediate", "DecodeFrame1",
		"SetProtectedResourceSession", "DecodeFrame2", "ProcessFrames1",
		"BuildRaytracingAccelerationStructure", "EmitRaytracingAccelerationStructurePostbuildInfo",
		"CopyRaytracingAccelerationStructure", "DispatchRays", "InitializeMetaCommand",
		"ExecuteMetaCommand", "EstimateMotion", "ResolveMotionVectorHeap", "SetPipelineState1",
		"InitializeExtensionCommand", "ExecuteExtensionCommand", "DispatchMesh", "EncodeFrame",
		"ResolveEncoderOutputMetadata", "Barrier", "BeginCommandList", "DispatchGraph", "SetProgram",
	};
	return (unsigned)op < sizeof(names) / sizeof(names[0]) ? names[op] : "another operation";
}

const char* ReasonName(HRESULT hr)
{
	switch (hr)
	{
	case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
	case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
	case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
	case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
	case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
	case S_OK: return "not removed";
	default: return "another reason";
	}
}

// A command list or queue's debug name, or what DRED has instead.
void Name(char* out, size_t size, const char* a, const wchar_t* w)
{
	if (w && *w)
		WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)size, nullptr, nullptr);
	else
		strncpy_s(out, size, a && *a ? a : "unnamed", _TRUNCATE);
}

void LogDred(ID3D12Device* device)
{
	ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
	if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))))
	{
		EchoXR_Log("  DRED: not available on this system");
		return;
	}

	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs = {};
	if (FAILED(dred->GetAutoBreadcrumbsOutput1(&crumbs)))
		EchoXR_Log("  DRED breadcrumbs: off (the game created its device without them)");
	else
	{
		// Only the lists the GPU started and didn't finish: one of them is what it hung on.
		int listed = 0, finished = 0;
		for (const D3D12_AUTO_BREADCRUMB_NODE1* n = crumbs.pHeadAutoBreadcrumbNode; n; n = n->pNext)
		{
			const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
			if (done >= n->BreadcrumbCount)
			{
				++finished;
				continue;
			}
			if (++listed > 8)
				continue;
			char list[96], queue[96];
			Name(list, sizeof(list), n->pCommandListDebugNameA, n->pCommandListDebugNameW);
			Name(queue, sizeof(queue), n->pCommandQueueDebugNameA, n->pCommandQueueDebugNameW);
			EchoXR_Log("  DRED: list \"%s\" on queue \"%s\" stopped at %u of %u, at %s", list, queue,
				done, n->BreadcrumbCount, done < n->BreadcrumbCount ? OpName(n->pCommandHistory[done]) : "its end");
			for (UINT c = 0; c < n->BreadcrumbContextsCount && n->pBreadcrumbContexts; ++c)
			{
				const D3D12_DRED_BREADCRUMB_CONTEXT& ctx = n->pBreadcrumbContexts[c];
				if (ctx.BreadcrumbIndex + 2 >= done && ctx.BreadcrumbIndex <= done + 1 && ctx.pContextString)
				{
					char text[160];
					Name(text, sizeof(text), nullptr, ctx.pContextString);
					EchoXR_Log("    at %u: %s", ctx.BreadcrumbIndex, text);
				}
			}
		}
		EchoXR_Log("  DRED: %d command list(s) unfinished, %d finished", listed, finished);
	}

	D3D12_DRED_PAGE_FAULT_OUTPUT1 fault = {};
	if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA)
	{
		EchoXR_Log("  DRED: page fault at GPU address 0x%llx", (unsigned long long)fault.PageFaultVA);
		int shown = 0;
		for (const D3D12_DRED_ALLOCATION_NODE1* a = fault.pHeadExistingAllocationNode; a && shown < 4; a = a->pNext, ++shown)
		{
			char name[96];
			Name(name, sizeof(name), a->ObjectNameA, a->ObjectNameW);
			EchoXR_Log("    existing allocation there: \"%s\" (type %d)", name, (int)a->AllocationType);
		}
		for (const D3D12_DRED_ALLOCATION_NODE1* a = fault.pHeadRecentFreedAllocationNode; a && shown < 8; a = a->pNext, ++shown)
		{
			char name[96];
			Name(name, sizeof(name), a->ObjectNameA, a->ObjectNameW);
			EchoXR_Log("    recently freed there: \"%s\" (type %d)", name, (int)a->AllocationType);
		}
	}
}

// Once per run, from whichever notices first: the fence watch or Echo VR's breakpoint.
void LogRemoval(ID3D12Device* device, const char* noticedBy)
{
	const HRESULT reason = device->GetDeviceRemovedReason();
	if (reason == S_OK || g_removalLogged.exchange(true))
		return;
	const char* call = g_call.load();
	EchoXR_Log("The graphics device was removed (%s, 0x%08lX), noticed by %s; OpenXR call in progress: %s; "
		"%llu slow frame call(s), %llu swapchain wait(s) past a frame, %llu given up", ReasonName(reason),
		(unsigned long)reason, noticedBy, call ? call : "none", (unsigned long long)g_slowCalls.load(),
		(unsigned long long)g_timedOutWaits.load(), (unsigned long long)g_gaveUpWaits.load());
	LogDred(device);
}

void CALLBACK OnRemoved(PVOID, BOOLEAN)
{
	std::lock_guard<std::mutex> lock(g_watchLock);
	if (g_device)
		LogRemoval(g_device, "EchoXR's watch");
}

LONG CALLBACK OnBreakpoint(EXCEPTION_POINTERS* e)
{
	// Not under g_watchLock: Echo VR may stop while the watch callback holds it. The device stays
	// referenced until EchoXR_UnwatchDevice removes this handler first.
	if (e->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT && g_device && !g_removalLogged.load())
		LogRemoval(g_device, "the game's own check");
	return EXCEPTION_CONTINUE_SEARCH;
}

void LogAdapter(ID3D12Device* device, LUID required)
{
	const LUID used = device->GetAdapterLuid();
	char name[128] = "unknown adapter";
	ComPtr<IDXGIFactory4> factory;
	ComPtr<IDXGIAdapter1> adapter;
	if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
		SUCCEEDED(factory->EnumAdapterByLuid(used, IID_PPV_ARGS(&adapter))))
	{
		DXGI_ADAPTER_DESC1 desc;
		if (SUCCEEDED(adapter->GetDesc1(&desc)))
			WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
	}
	const bool same = used.LowPart == required.LowPart && used.HighPart == required.HighPart;
	EchoXR_Log("D3D12 device on %s (LUID %08lx:%08lx); the OpenXR runtime asked for %08lx:%08lx%s", name,
		(unsigned long)used.HighPart, used.LowPart, (unsigned long)required.HighPart, required.LowPart,
		same ? "" : " -- NOT THE SAME ADAPTER");
}

} // namespace

EchoXR_Call::EchoXR_Call(const char* name)
	: m_Name(name)
	, m_Outer(g_call.exchange(name))
	, m_Start(GetTickCount64())
{
}

EchoXR_Call::~EchoXR_Call()
{
	g_call.store(m_Outer);
	const ULONGLONG ms = GetTickCount64() - m_Start;
	if (ms >= 200)
	{
		const uint64_t n = ++g_slowCalls;
		if (echoxr::LogNth(n))
			EchoXR_Log("Slow frame call: %s took %llu ms (%llu so far)", m_Name, ms, (unsigned long long)n);
	}
}

void EchoXR_SwapchainWaitTimedOut(int64_t waitedNs, bool gaveUp)
{
	const uint64_t n = gaveUp ? ++g_gaveUpWaits : ++g_timedOutWaits;
	if (!echoxr::LogNth(n))
		return;
	if (gaveUp)
		EchoXR_Log("A swapchain image wasn't ready after %lld ms; going on without it (%llu so far)",
			(long long)(waitedNs / 1000000), (unsigned long long)n);
	else
		EchoXR_Log("A swapchain image wasn't ready within a frame; drawing into it anyway "
			"(SwapchainWait = frame, %llu so far)", (unsigned long long)n);
}

void EchoXR_EnableDred()
{
	ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
	if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred))))
	{
		EchoXR_Log("DRED: not available on this system");
		return;
	}
	dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
	dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
}

void EchoXR_WatchD3D12Device(ID3D12Device* device, LUID required)
{
	LogAdapter(device, required);

	std::lock_guard<std::mutex> lock(g_watchLock);
	if (g_device)
		return;
	ComPtr<ID3D12Fence> fence;
	HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	HANDLE wait = nullptr;
	// A removed device completes every fence with UINT64_MAX, which this one never reaches
	// otherwise: the documented way to hear of a removal without polling.
	if (!event || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
		FAILED(fence->SetEventOnCompletion(UINT64_MAX, event)) ||
		!RegisterWaitForSingleObject(&wait, event, OnRemoved, nullptr, INFINITE, WT_EXECUTEONLYONCE))
	{
		EchoXR_Log("Couldn't watch the graphics device for a removal; only the game's own check will log one");
		if (event)
			CloseHandle(event);
		event = nullptr;
		wait = nullptr;
		fence.Reset();
	}
	device->AddRef();
	g_device = device;
	g_fence = fence.Detach();
	g_removedEvent = event;
	g_wait = wait;
	g_breakpointHandler = AddVectoredExceptionHandler(1, OnBreakpoint);
}

void EchoXR_UnwatchDevice()
{
	if (g_breakpointHandler)
		RemoveVectoredExceptionHandler(g_breakpointHandler);
	g_breakpointHandler = nullptr;
	// Waits for a running callback, outside the lock it takes.
	if (g_wait)
		UnregisterWaitEx(g_wait, INVALID_HANDLE_VALUE);
	g_wait = nullptr;

	std::lock_guard<std::mutex> lock(g_watchLock);
	if (g_removedEvent)
		CloseHandle(g_removedEvent);
	g_removedEvent = nullptr;
	if (g_fence)
		g_fence->Release();
	g_fence = nullptr;
	if (g_device)
		g_device->Release();
	g_device = nullptr;
}
