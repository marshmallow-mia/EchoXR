// GPU-side waits on D3D12 queues (see echoxr_diag.h). A hang on SteamVR's D3D12 path left
// every unfinished command list at its start (DRED: "stopped at 0 of N, at BeginCommandList"):
// the queue was held between lists, by a Wait on a fence nobody signalled. DRED doesn't say
// whose wait it was; these lines do.
#include "echoxr_diag.h"
#include "echoxr_policy.h"

#include <d3d12.h>
#include <detours/detours.h>
#include <intrin.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <mutex>

using Microsoft::WRL::ComPtr;

void EchoXR_Log(const char* fmt, ...);

namespace {

using QueueFenceCall = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
QueueFenceCall TrueSignal = nullptr;
QueueFenceCall TrueWait = nullptr;
constexpr UINT kSignalSlot = 14, kWaitSlot = 15;   // in ID3D12CommandQueue's vtable

constexpr int kPending = 256;
constexpr int kSignalled = 64;
constexpr ULONGLONG kStuckMs = 500;   // a frame is 11-14 ms; Windows calls the GPU hung at 2 s
constexpr DWORD kLookMs = 100;

// A Wait the GPU may still be held by. The fence is referenced while it is listed.
struct Pending
{
	char queueName[48];
	bool gameQueue;
	ID3D12Fence* fence;
	UINT64 value;
	UINT64 seen;                 // the fence's value at the last look before a removal
	void* caller;
	ULONGLONG since;
	bool logged;
};

// The last value a queue in this process was asked to signal a fence with.
struct Signalled
{
	ID3D12Fence* fence;          // only compared
	char queueName[48];
	UINT64 value;
	void* caller;
};

std::mutex g_lock;
Pending g_pending[kPending];
int g_pendingCount = 0;
Signalled g_signalled[kSignalled];
int g_signalledNext = 0;
uint64_t g_overflow = 0;
uint64_t g_stuck = 0;
ID3D12CommandQueue* g_gameQueue = nullptr;
ID3D12Device* g_device = nullptr;   // referenced while watched
HANDLE g_timer = nullptr;
bool g_frozen = false;              // the device is gone: the list stays as it was
bool g_hooked = false;
std::atomic<bool> g_on{ false };

void QueueName(ID3D12CommandQueue* queue, char* out, size_t size)
{
	wchar_t name[48] = {};
	UINT bytes = sizeof(name) - sizeof(wchar_t);
	if (SUCCEEDED(queue->GetPrivateData(WKPDID_D3DDebugObjectNameW, &bytes, name)) && name[0])
		WideCharToMultiByte(CP_UTF8, 0, name, -1, out, (int)size, nullptr, nullptr);
	else
		strncpy_s(out, size, "unnamed", _TRUNCATE);
}

// The module and offset of `address`, such as "vrclient_x64.dll+0x1a2b3".
void Where(void* address, char* out, size_t size)
{
	HMODULE module = nullptr;
	wchar_t path[MAX_PATH];
	if (address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCWSTR)address, &module) && GetModuleFileNameW(module, path, MAX_PATH))
	{
		const wchar_t* base = wcsrchr(path, L'\\');
		char name[96];
		WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : path, -1, name, sizeof(name), nullptr, nullptr);
		snprintf(out, size, "%s+0x%llx", name, (unsigned long long)((char*)address - (char*)module));
	}
	else
		snprintf(out, size, "%p", address);
}

// Drops the waits whose fence got there. A fence at UINT64_MAX belongs to a removed device:
// its wait stays as it was last seen.
void Sweep()
{
	int kept = 0;
	for (int i = 0; i < g_pendingCount; ++i)
	{
		Pending& p = g_pending[i];
		const UINT64 now = p.fence->GetCompletedValue();
		if (now != UINT64_MAX && now >= p.value)
		{
			p.fence->Release();
			continue;
		}
		if (now != UINT64_MAX)
			p.seen = now;
		g_pending[kept++] = p;
	}
	g_pendingCount = kept;
}

// One wait as a line's text, with the last signal of its fence asked of a queue here.
void Describe(const Pending& p, const Signalled* last, ULONGLONG now, char* out, size_t size)
{
	char caller[128], signal[224];
	Where(p.caller, caller, sizeof(caller));
	if (last)
	{
		char by[128];
		Where(last->caller, by, sizeof(by));
		snprintf(signal, sizeof(signal), "the last signal of that fence asked of a queue here: %llu on \"%s\", by %s",
			(unsigned long long)last->value, last->queueName, by);
	}
	else
		snprintf(signal, sizeof(signal), "no queue here was asked to signal it (the CPU or another process does)");
	snprintf(out, size, "queue \"%s\"%s waits for fence %p to reach %llu (it was at %llu), %llu ms so far, queued by %s; %s",
		p.queueName, p.gameQueue ? " (the game's)" : "", (void*)p.fence, (unsigned long long)p.value,
		(unsigned long long)p.seen, (unsigned long long)(now - p.since), caller, signal);
}

// Under g_lock.
const Signalled* LastSignal(ID3D12Fence* fence)
{
	for (const Signalled& s : g_signalled)
		if (s.fence == fence)
			return &s;
	return nullptr;
}

HRESULT STDMETHODCALLTYPE HookWait(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value)
{
	if (g_on.load(std::memory_order_relaxed) && fence)
	{
		const UINT64 have = fence->GetCompletedValue();
		if (have < value)
		{
			void* caller = _ReturnAddress();
			std::lock_guard<std::mutex> lock(g_lock);
			if (g_on.load() && !g_frozen)
			{
				if (g_pendingCount == kPending)
					Sweep();
				if (g_pendingCount == kPending)
					++g_overflow;
				else
				{
					Pending& p = g_pending[g_pendingCount++];
					p.gameQueue = queue == g_gameQueue;
					QueueName(queue, p.queueName, sizeof(p.queueName));
					fence->AddRef();
					p.fence = fence;
					p.value = value;
					p.seen = have;
					p.caller = caller;
					p.since = GetTickCount64();
					p.logged = false;
				}
			}
		}
	}
	return TrueWait(queue, fence, value);
}

HRESULT STDMETHODCALLTYPE HookSignal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value)
{
	if (g_on.load(std::memory_order_relaxed) && fence)
	{
		void* caller = _ReturnAddress();
		std::lock_guard<std::mutex> lock(g_lock);
		if (g_on.load() && !g_frozen)
		{
			Signalled* s = const_cast<Signalled*>(LastSignal(fence));
			if (!s)
			{
				s = &g_signalled[g_signalledNext];
				g_signalledNext = (g_signalledNext + 1) % kSignalled;
			}
			s->fence = fence;
			QueueName(queue, s->queueName, sizeof(s->queueName));
			s->value = value;
			s->caller = caller;
		}
	}
	return TrueSignal(queue, fence, value);
}

// Every 100 ms: drops the waits that are over and logs the ones pending for half a second.
void CALLBACK Look(PVOID, BOOLEAN)
{
	Pending stuck[4];
	Signalled last[4];
	bool hasLast[4] = {};
	uint64_t count[4];
	int n = 0;
	const ULONGLONG now = GetTickCount64();
	{
		std::lock_guard<std::mutex> lock(g_lock);
		if (!g_on.load() || g_frozen)
			return;
		if (g_device->GetDeviceRemovedReason() != S_OK)
		{
			g_frozen = true;
			return;
		}
		Sweep();
		for (int i = 0; i < g_pendingCount; ++i)
		{
			Pending& p = g_pending[i];
			if (p.logged || now - p.since < kStuckMs)
				continue;
			p.logged = true;
			if (!echoxr::LogNth(++g_stuck) || n == 4)
				continue;
			stuck[n] = p;
			count[n] = g_stuck;
			if (const Signalled* s = LastSignal(p.fence))
			{
				last[n] = *s;
				hasLast[n] = true;
			}
			++n;
		}
	}
	// Module names take the loader lock: outside g_lock. The fence is only printed.
	for (int i = 0; i < n; ++i)
	{
		char text[640];
		Describe(stuck[i], hasLast[i] ? &last[i] : nullptr, now, text, sizeof(text));
		EchoXR_Log("A GPU wait is still pending after half a second (%llu so far): %s",
			(unsigned long long)count[i], text);
	}
}

} // namespace

void EchoXR_WatchD3D12Queue(ID3D12CommandQueue* queue)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (g_on.load() || !queue)
		return;
	ComPtr<ID3D12Device> device;
	if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
		return;
	if (!g_hooked)
	{
		// On the driver's methods, so every queue in the process is seen (SteamVR's too); the
		// hooks stay for the process, idle while nothing is watched.
		void** vtable = *(void***)queue;
		TrueSignal = (QueueFenceCall)vtable[kSignalSlot];
		TrueWait = (QueueFenceCall)vtable[kWaitSlot];
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach((PVOID*)&TrueSignal, (PVOID)HookSignal);
		DetourAttach((PVOID*)&TrueWait, (PVOID)HookWait);
		if (DetourTransactionCommit() != NO_ERROR)
		{
			EchoXR_Log("Couldn't watch the GPU's waits (the queue's methods couldn't be hooked)");
			return;
		}
		g_hooked = true;
	}
	if (!CreateTimerQueueTimer(&g_timer, nullptr, Look, nullptr, kLookMs, kLookMs, WT_EXECUTEDEFAULT))
	{
		g_timer = nullptr;
		EchoXR_Log("Couldn't watch the GPU's waits (no timer)");
		return;
	}
	g_device = device.Detach();
	g_gameQueue = queue;
	g_frozen = false;
	g_overflow = 0;
	g_stuck = 0;
	g_on.store(true);
	EchoXR_Log("Watching the GPU's waits on the game's queue and every other D3D12 queue here");
}

void EchoXR_LogQueueWaits()
{
	Pending shown[8];
	Signalled last[8];
	bool hasLast[8] = {};
	int total = 0, n = 0;
	uint64_t overflow = 0;
	const ULONGLONG now = GetTickCount64();
	{
		std::lock_guard<std::mutex> lock(g_lock);
		if (!g_on.load())
		{
			EchoXR_Log("  GPU waits: not watched");
			return;
		}
		g_frozen = true;
		total = g_pendingCount;
		overflow = g_overflow;
		// Oldest first: the list keeps the order the waits came in.
		for (; n < total && n < 8; ++n)
		{
			shown[n] = g_pending[n];
			if (const Signalled* s = LastSignal(shown[n].fence))
			{
				last[n] = *s;
				hasLast[n] = true;
			}
		}
	}
	if (total == 0)
		EchoXR_Log("  GPU waits: none pending (the GPU was held inside a command list, or by a queue of another kind)");
	else
		EchoXR_Log("  GPU waits pending at the removal: %d (oldest first)", total);
	for (int i = 0; i < n; ++i)
	{
		char text[640];
		Describe(shown[i], hasLast[i] ? &last[i] : nullptr, now, text, sizeof(text));
		EchoXR_Log("    %s", text);
	}
	if (overflow)
		EchoXR_Log("  GPU waits not followed: %llu (more than %d pending at once)", (unsigned long long)overflow, kPending);
}

void EchoXR_UnwatchQueues()
{
	HANDLE timer;
	{
		std::lock_guard<std::mutex> lock(g_lock);
		g_on.store(false);
		timer = g_timer;
		g_timer = nullptr;
	}
	// Waits for a running look, which takes g_lock: outside it.
	if (timer)
		DeleteTimerQueueTimer(nullptr, timer, INVALID_HANDLE_VALUE);

	std::lock_guard<std::mutex> lock(g_lock);
	for (int i = 0; i < g_pendingCount; ++i)
		g_pending[i].fence->Release();
	g_pendingCount = 0;
	for (Signalled& s : g_signalled)
		s = Signalled{};
	g_signalledNext = 0;
	if (g_device)
		g_device->Release();
	g_device = nullptr;
	g_gameQueue = nullptr;
}
