// What runtime.log says about a graphics device that stops responding (see echoxr_diag.h).
#include "echoxr_diag.h"
#include "echoxr_policy.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

void EchoXR_Log(const char* fmt, ...);

namespace {

std::atomic<int> g_verbose{ -1 };
std::atomic<int> g_important{ 0 };

// The frame calls timed for the line a second: the second's count, total and longest.
struct CallStats
{
	const char* name;
	std::atomic<uint64_t> count{ 0 }, totalUs{ 0 }, longestUs{ 0 };
};
CallStats g_stats[] = { { "xrWaitFrame" }, { "xrBeginFrame" }, { "xrEndFrame" }, { "xrWaitSwapchainImage" } };

LONGLONG Ticks()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

uint64_t TicksToUs(LONGLONG ticks)
{
	static const LONGLONG perSecond = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
	return ticks <= 0 ? 0 : (uint64_t)(ticks * 1000000 / perSecond);
}

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

// The game's adapter, for its video memory; a timer looks at it once a second.
std::mutex g_adapterLock;
IDXGIAdapter3* g_adapter = nullptr;
HANDLE g_tick = nullptr;
uint64_t g_seconds = 0;
UINT64 g_loggedBudget = 0;
uint64_t g_budgetChanges = 0;
bool g_past90 = false, g_past100 = false;
bool g_adaptersLogged = false;

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

// The breadcrumb ops from `from` to `to` (exclusive), the one at `at` in brackets.
std::string Ops(const D3D12_AUTO_BREADCRUMB_NODE1* n, UINT from, UINT to, UINT at)
{
	std::string out;
	for (UINT i = from; i < to && n->pCommandHistory; ++i)
	{
		if (!out.empty())
			out += ", ";
		out += i == at ? "[" : "";
		out += OpName(n->pCommandHistory[i]);
		out += i == at ? "]" : "";
	}
	return out;
}

void LogDred(ID3D12Device* device)
{
	ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
	if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))))
	{
		EchoXR_Log("  DRED: not available on this system");
		return;
	}
	const bool max = EchoXR_Max();

	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs = {};
	if (FAILED(dred->GetAutoBreadcrumbsOutput1(&crumbs)))
		EchoXR_Log("  DRED breadcrumbs: off (the game created its device without them)");
	else
	{
		// The lists the GPU didn't finish: one of them is what it hung on. The ones it started
		// come first, as they are the likeliest; then per queue how many it finished.
		struct Queue { std::string name; int unfinished = 0, started = 0, finished = 0; };
		std::vector<Queue> queues;
		std::vector<const D3D12_AUTO_BREADCRUMB_NODE1*> unfinished;
		int finished = 0;
		for (const D3D12_AUTO_BREADCRUMB_NODE1* n = crumbs.pHeadAutoBreadcrumbNode; n; n = n->pNext)
		{
			const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
			char queue[96];
			Name(queue, sizeof(queue), n->pCommandQueueDebugNameA, n->pCommandQueueDebugNameW);
			auto q = std::find_if(queues.begin(), queues.end(), [&](const Queue& x) { return x.name == queue; });
			if (q == queues.end())
			{
				queues.push_back({ queue });
				q = queues.end() - 1;
			}
			if (done >= n->BreadcrumbCount)
			{
				++finished;
				++q->finished;
				continue;
			}
			++q->unfinished;
			q->started += done > 0;
			unfinished.push_back(n);
		}
		std::stable_partition(unfinished.begin(), unfinished.end(), [](const D3D12_AUTO_BREADCRUMB_NODE1* n) {
			return n->pLastBreadcrumbValue && *n->pLastBreadcrumbValue > 0;
		});

		int listed = 0;
		for (const D3D12_AUTO_BREADCRUMB_NODE1* n : unfinished)
		{
			if (!max && ++listed > 8)
				break;
			const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
			char list[96], queue[96];
			Name(list, sizeof(list), n->pCommandListDebugNameA, n->pCommandListDebugNameW);
			Name(queue, sizeof(queue), n->pCommandQueueDebugNameA, n->pCommandQueueDebugNameW);
			EchoXR_Log("  DRED: list \"%s\" on queue \"%s\" stopped at %u of %u, at %s", list, queue,
				done, n->BreadcrumbCount, OpName(n->pCommandHistory[done]));
			if (max)
			{
				const UINT from = done > 8 ? done - 8 : 0;
				const UINT to = std::min<UINT>(n->BreadcrumbCount, done + 9);
				EchoXR_Log("    ops %u-%u: %s", from, to - 1, Ops(n, from, to, done).c_str());
			}
			for (UINT c = 0; c < n->BreadcrumbContextsCount && n->pBreadcrumbContexts; ++c)
			{
				const D3D12_DRED_BREADCRUMB_CONTEXT& ctx = n->pBreadcrumbContexts[c];
				if (ctx.pContextString && (max || (ctx.BreadcrumbIndex + 2 >= done && ctx.BreadcrumbIndex <= done + 1)))
				{
					char text[160];
					Name(text, sizeof(text), nullptr, ctx.pContextString);
					EchoXR_Log("    at %u: %s", ctx.BreadcrumbIndex, text);
				}
			}
		}
		if (!max && unfinished.size() > 8)
			EchoXR_Log("  DRED: %d more unfinished list(s) not shown (ECHOXR_VERBOSE=max shows all)",
				(int)unfinished.size() - 8);
		for (const Queue& q : queues)
			EchoXR_Log("  DRED: queue \"%s\": %d list(s) unfinished (%d of them started), %d finished", q.name.c_str(),
				q.unfinished, q.started, q.finished);
		EchoXR_Log("  DRED: %d command list(s) unfinished, %d finished", (int)unfinished.size(), finished);
	}

	D3D12_DRED_PAGE_FAULT_OUTPUT1 fault = {};
	if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA)
	{
		EchoXR_Log("  DRED: page fault at GPU address 0x%llx", (unsigned long long)fault.PageFaultVA);
		int shown = 0;
		for (const D3D12_DRED_ALLOCATION_NODE1* a = fault.pHeadExistingAllocationNode; a && (max || shown < 4); a = a->pNext, ++shown)
		{
			char name[96];
			Name(name, sizeof(name), a->ObjectNameA, a->ObjectNameW);
			EchoXR_Log("    existing allocation there: \"%s\" (type %d)", name, (int)a->AllocationType);
		}
		for (const D3D12_DRED_ALLOCATION_NODE1* a = fault.pHeadRecentFreedAllocationNode; a && (max || shown < 8); a = a->pNext, ++shown)
		{
			char name[96];
			Name(name, sizeof(name), a->ObjectNameA, a->ObjectNameW);
			EchoXR_Log("    recently freed there: \"%s\" (type %d)", name, (int)a->AllocationType);
		}
	}
	else if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)))
		EchoXR_Log("  DRED: no page fault");
}

double Mb(UINT64 bytes) { return bytes / (1024.0 * 1024.0); }

// The game adapter's video memory as words: "4890 of 5420 MB budget (90%), 120 MB reserved;
// shared: 310 of 7900 MB". False when it can't be read.
bool VideoMemory(IDXGIAdapter3* adapter, char* out, size_t size, DXGI_QUERY_VIDEO_MEMORY_INFO* localOut = nullptr)
{
	DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, shared = {};
	if (!adapter || FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
		return false;
	adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
	snprintf(out, size, "%.0f of %.0f MB budget (%.0f%%), %.0f MB reserved; shared: %.0f of %.0f MB",
		Mb(local.CurrentUsage), Mb(local.Budget), local.Budget ? 100.0 * local.CurrentUsage / local.Budget : 0.0,
		Mb(local.CurrentReservation), Mb(shared.CurrentUsage), Mb(shared.Budget));
	if (localOut)
		*localOut = local;
	return true;
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
	g_important.fetch_add(1);
	char memory[256];
	ComPtr<IDXGIAdapter3> adapter;
	{
		// The session's end may be releasing it: a reference of its own. (The watch callback
		// holds g_watchLock here.)
		std::lock_guard<std::mutex> lock(g_adapterLock);
		adapter = g_adapter;
	}
	if (VideoMemory(adapter.Get(), memory, sizeof(memory)))
		EchoXR_Log("  Video memory at the removal: %s", memory);
	LogDred(device);
	EchoXR_LogQueueWaits();
	EchoXR_LogResources();
	g_important.fetch_sub(1);
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

// Every adapter here, with its memory and driver version (NVIDIA's own number too: 32.0.15.6631
// is 566.31).
void LogAdapters()
{
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return;
	ComPtr<IDXGIAdapter1> a;
	for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset())
	{
		DXGI_ADAPTER_DESC1 d = {};
		if (!a || FAILED(a->GetDesc1(&d)))
			continue;
		char name[128];
		WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
		char driver[64] = "unknown";
		LARGE_INTEGER umd = {};
		if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
		{
			const unsigned v[4] = { HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart) };
			const int n = snprintf(driver, sizeof(driver), "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
			if (d.VendorId == 0x10de && n > 0)
			{
				const unsigned nv = (v[2] % 10) * 10000 + v[3];
				snprintf(driver + n, sizeof(driver) - n, " (NVIDIA %u.%02u)", nv / 100, nv % 100);
			}
		}
		EchoXR_Log("Adapter %u: %s, vendor %04x device %04x, %.0f MB of its own memory, %.0f MB shared, driver %s%s", i,
			name, d.VendorId, d.DeviceId, Mb(d.DedicatedVideoMemory), Mb(d.SharedSystemMemory), driver,
			(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "");
	}
}

void LogCommandLine()
{
	char line[1024];
	WideCharToMultiByte(CP_UTF8, 0, GetCommandLineW(), -1, line, sizeof(line), nullptr, nullptr);
	line[sizeof(line) - 1] = 0;
	EchoXR_Log("Command line: %s", line);
}

// "xrWaitFrame 90 calls, 9.8 ms on average, 14.1 at most; ..." for the second, which starts over.
void CallsOfTheSecond(char* out, size_t size)
{
	size_t used = 0;
	out[0] = 0;
	for (CallStats& c : g_stats)
	{
		const uint64_t n = c.count.exchange(0), total = c.totalUs.exchange(0), longest = c.longestUs.exchange(0);
		const int w = snprintf(out + used, size - used, "%s%s %llu, %.1f ms on average, %.1f at most", used ? "; " : "",
			c.name, (unsigned long long)n, n ? total / 1000.0 / n : 0.0, longest / 1000.0);
		if (w < 0 || (size_t)w >= size - used)
			break;
		used += w;
	}
}

std::atomic_flag g_ticking = ATOMIC_FLAG_INIT;

// Once a second: the video memory's budget and use, and at max a line with everything counted.
void CALLBACK Tick(PVOID, BOOLEAN)
{
	if (g_ticking.test_and_set())
		return;     // the last one is still writing
	ComPtr<IDXGIAdapter3> adapter;
	{
		std::lock_guard<std::mutex> lock(g_adapterLock);
		adapter = g_adapter;
	}
	// After a removal its report has said it all.
	if (adapter && !g_removalLogged.load())
	{
		const uint64_t second = ++g_seconds;
		DXGI_QUERY_VIDEO_MEMORY_INFO local = {};
		char memory[256] = "unknown";
		if (VideoMemory(adapter.Get(), memory, sizeof(memory), &local) && local.Budget)
		{
			if (g_loggedBudget && (local.Budget > g_loggedBudget / 20 * 21 || local.Budget < g_loggedBudget / 20 * 19))
			{
				g_loggedBudget = local.Budget;
				const uint64_t n = ++g_budgetChanges;
				if (EchoXR_LogNth(n))
					EchoXR_Log("The video memory budget changed (%llu so far): %s", (unsigned long long)n, memory);
			}
			if (!g_past90 && local.CurrentUsage * 10 >= local.Budget * 9)
			{
				g_past90 = true;
				EchoXR_Log("Video memory use passed 90%% of the budget: %s", memory);
			}
			if (!g_past100 && local.CurrentUsage >= local.Budget)
			{
				g_past100 = true;
				EchoXR_Log("Video memory use passed the budget (Windows moves memory out to shared now): %s", memory);
			}
		}
		if (EchoXR_Max())
		{
			char calls[512], queues[2048], resources[256];
			CallsOfTheSecond(calls, sizeof(calls));
			EchoXR_QueueSecond(queues, sizeof(queues));
			EchoXR_ResourceSecond(resources, sizeof(resources));
			EchoXR_Log("Second %llu: %s; video memory %s; %s; %s", (unsigned long long)second, calls, memory, queues,
				resources);
		}
	}
	g_ticking.clear();
}

// The adapters, the game adapter's video memory and the tick, once per watch.
void StartTick(ID3D12Device* device)
{
	if (!g_adaptersLogged)
	{
		g_adaptersLogged = true;
		LogAdapters();
		if (EchoXR_Max())
			LogCommandLine();
	}
	ComPtr<IDXGIFactory4> factory;
	ComPtr<IDXGIAdapter3> adapter;
	DXGI_QUERY_VIDEO_MEMORY_INFO local = {};
	char memory[256];
	if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
		SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
		VideoMemory(adapter.Get(), memory, sizeof(memory), &local))
		EchoXR_Log("Video memory of the game's adapter: %s", memory);
	else
	{
		EchoXR_Log("Video memory: this system doesn't say how much the game uses");
		adapter.Reset();
	}
	g_loggedBudget = local.Budget;
	g_past90 = g_past100 = false;
	g_seconds = 0;
	g_budgetChanges = 0;
	{
		std::lock_guard<std::mutex> lock(g_adapterLock);
		g_adapter = adapter.Detach();
	}
	if (!CreateTimerQueueTimer(&g_tick, nullptr, Tick, nullptr, 1000, 1000, WT_EXECUTEDEFAULT))
		g_tick = nullptr;
}

// Under g_watchLock.
void WatchRemoval(ID3D12Device* device)
{
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

} // namespace

echoxr::Verbose EchoXR_Verbose()
{
	int v = g_verbose.load(std::memory_order_relaxed);
	if (v < 0)
	{
		echoxr::Verbose level = echoxr::Verbose::Default;
		char text[32];
		const DWORD n = GetEnvironmentVariableA("ECHOXR_VERBOSE", text, sizeof(text));
		if (n > 0 && n < sizeof(text))
			echoxr::ParseVerbose(text, level);
		v = (int)level;
		g_verbose.store(v);
	}
	return (echoxr::Verbose)v;
}

bool EchoXR_LogNth(uint64_t count)
{
	return EchoXR_Max() || echoxr::LogNth(count);
}

bool EchoXR_LogImportant()
{
	return g_important.load() > 0;
}

EchoXR_Call::EchoXR_Call(const char* name)
	: m_Name(name)
	, m_Outer(g_call.exchange(name))
	, m_Start(Ticks())
{
}

EchoXR_Call::~EchoXR_Call()
{
	g_call.store(m_Outer);
	const uint64_t us = TicksToUs(Ticks() - m_Start);
	for (CallStats& c : g_stats)
		if (strcmp(c.name, m_Name) == 0)
		{
			++c.count;
			c.totalUs += us;
			uint64_t longest = c.longestUs.load();
			while (us > longest && !c.longestUs.compare_exchange_weak(longest, us))
				;
			break;
		}
	const uint64_t ms = us / 1000;
	if (ms >= (EchoXR_Max() ? 50u : 200u))
	{
		const uint64_t n = ++g_slowCalls;
		if (EchoXR_LogNth(n))
			EchoXR_Log("Slow frame call: %s took %llu ms (%llu so far)", m_Name, (unsigned long long)ms,
				(unsigned long long)n);
	}
}

void EchoXR_SwapchainWaitTimedOut(int64_t waitedNs, bool gaveUp)
{
	const uint64_t n = gaveUp ? ++g_gaveUpWaits : ++g_timedOutWaits;
	if (!EchoXR_LogNth(n))
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
	{
		std::lock_guard<std::mutex> lock(g_watchLock);
		if (g_device)
			return;
		WatchRemoval(device);
	}
	StartTick(device);
	EchoXR_WatchD3D12Resources(device);
}

void EchoXR_UnwatchDevice()
{
	EchoXR_UnwatchQueues();
	// Waits for a running tick.
	if (g_tick)
		DeleteTimerQueueTimer(nullptr, g_tick, INVALID_HANDLE_VALUE);
	g_tick = nullptr;
	EchoXR_UnwatchResources();
	{
		std::lock_guard<std::mutex> lock(g_adapterLock);
		if (g_adapter)
			g_adapter->Release();
		g_adapter = nullptr;
	}
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
