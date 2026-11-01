// GPU-side waits on D3D12 queues (see echoxr_diag.h). A hang on SteamVR's D3D12 path left
// every unfinished command list at its start (DRED: "stopped at 0 of N, at BeginCommandList"):
// the queue was held between lists, by a Wait on a fence nobody signalled. DRED doesn't say
// whose wait it was; these lines do. They also count, per queue, the work it was given
// (ExecuteCommandLists) and the signals it was asked for, for the removal's report and the line
// a second at max.
#include "echoxr_diag.h"
#include "echoxr_policy.h"

#include <d3d12.h>
#include <detours/detours.h>
#include <intrin.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>

using Microsoft::WRL::ComPtr;

void EchoXR_Log(const char* fmt, ...);

namespace {

using QueueFenceCall = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
using ExecuteCall = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
QueueFenceCall TrueSignal = nullptr;
QueueFenceCall TrueWait = nullptr;
ExecuteCall TrueExecute = nullptr;
constexpr UINT kExecuteSlot = 10, kSignalSlot = 14, kWaitSlot = 15;   // in ID3D12CommandQueue's vtable

constexpr int kPending = 256;
constexpr int kSignalled = 64;
constexpr int kQueues = 16;
// A frame is 11-14 ms; Windows calls the GPU hung at 2 s.
ULONGLONG StuckMs() { return EchoXR_Max() ? 50 : 500; }
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

// What a queue here was given and asked for: since the watch began, and in this second.
struct QueueStats
{
	ID3D12CommandQueue* queue;   // only compared
	char name[48];
	uint64_t submissions, lists, signals, waits;
	uint64_t secondSubmissions, secondSignals, secondWaits;
	ULONGLONG lastSubmission, lastSignal;
	UINT lastLists;
	void* lastSubmitter;
};

std::mutex g_lock;
QueueStats g_queues[kQueues];
int g_queueCount = 0;
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

// Under g_lock: the queue's counts, made the first time it is seen (none past kQueues).
QueueStats* Stats(ID3D12CommandQueue* queue)
{
	for (int i = 0; i < g_queueCount; ++i)
		if (g_queues[i].queue == queue)
		{
			QueueStats& q = g_queues[i];
			// A name given after the queue's first use.
			if (strcmp(q.name, "unnamed") == 0 && (q.submissions + q.signals) % 64 == 0)
				QueueName(queue, q.name, sizeof(q.name));
			return &q;
		}
	if (g_queueCount == kQueues)
		return nullptr;
	QueueStats& q = g_queues[g_queueCount++];
	q = QueueStats{};
	q.queue = queue;
	QueueName(queue, q.name, sizeof(q.name));
	return &q;
}

} // namespace

void EchoXR_Where(void* address, char* out, size_t size)
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

namespace {

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
	EchoXR_Where(p.caller, caller, sizeof(caller));
	if (last)
	{
		char by[128];
		EchoXR_Where(last->caller, by, sizeof(by));
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
		{
			std::lock_guard<std::mutex> lock(g_lock);
			if (!g_frozen)
				if (QueueStats* q = Stats(queue))
				{
					++q->waits;
					++q->secondWaits;
				}
		}
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
			if (QueueStats* q = Stats(queue))
			{
				++q->signals;
				++q->secondSignals;
				q->lastSignal = GetTickCount64();
			}
		}
	}
	return TrueSignal(queue, fence, value);
}

void STDMETHODCALLTYPE HookExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
	if (g_on.load(std::memory_order_relaxed))
	{
		void* caller = _ReturnAddress();
		std::lock_guard<std::mutex> lock(g_lock);
		if (g_on.load() && !g_frozen)
			if (QueueStats* q = Stats(queue))
			{
				++q->submissions;
				++q->secondSubmissions;
				q->lists += count;
				q->lastSubmission = GetTickCount64();
				q->lastLists = count;
				q->lastSubmitter = caller;
			}
	}
	TrueExecute(queue, count, lists);
}

// Every 100 ms: drops the waits that are over and logs the ones pending for half a second.
void CALLBACK Look(PVOID, BOOLEAN)
{
	constexpr int kShown = 16;
	Pending stuck[kShown];
	Signalled last[kShown];
	bool hasLast[kShown] = {};
	uint64_t count[kShown];
	const int shown = EchoXR_Max() ? kShown : 4;
	const ULONGLONG stuckMs = StuckMs();
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
			if (p.logged || now - p.since < stuckMs)
				continue;
			p.logged = true;
			if (!EchoXR_LogNth(++g_stuck) || n == shown)
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
		EchoXR_Log("A GPU wait is still pending after %llu ms (%llu so far): %s", (unsigned long long)stuckMs,
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
		TrueExecute = (ExecuteCall)vtable[kExecuteSlot];
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach((PVOID*)&TrueSignal, (PVOID)HookSignal);
		DetourAttach((PVOID*)&TrueWait, (PVOID)HookWait);
		DetourAttach((PVOID*)&TrueExecute, (PVOID)HookExecute);
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
	g_queueCount = 0;
	g_on.store(true);
	EchoXR_Log("Watching the GPU's waits on the game's queue and every other D3D12 queue here");
}

void EchoXR_LogQueueWaits()
{
	// Static: at max every pending wait is listed, more than a thread's stack should hold. Only
	// the one removal report uses them.
	static Pending shown[kPending];
	static Signalled last[kPending];
	static bool hasLast[kPending];
	static Signalled ring[kSignalled];
	static QueueStats queues[kQueues];
	const bool max = EchoXR_Max();
	int total = 0, n = 0, queueCount = 0;
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
		for (; n < total && (max || n < 8); ++n)
		{
			shown[n] = g_pending[n];
			hasLast[n] = false;
			if (const Signalled* s = LastSignal(shown[n].fence))
			{
				last[n] = *s;
				hasLast[n] = true;
			}
		}
		for (int i = 0; i < kSignalled; ++i)
			ring[i] = g_signalled[(g_signalledNext + i) % kSignalled];
		queueCount = g_queueCount;
		for (int i = 0; i < queueCount; ++i)
			queues[i] = g_queues[i];
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
	if (n < total)
		EchoXR_Log("    %d more not shown (ECHOXR_VERBOSE=max shows all)", total - n);
	if (overflow)
		EchoXR_Log("  GPU waits not followed: %llu (more than %d pending at once)", (unsigned long long)overflow, kPending);

	// Per queue: when it was last given work and asked for a signal.
	for (int i = 0; i < queueCount; ++i)
	{
		const QueueStats& q = queues[i];
		char by[128] = "nobody";
		if (q.lastSubmitter)
			EchoXR_Where(q.lastSubmitter, by, sizeof(by));
		char submitted[96] = "never given work", signalled[64] = "never asked to signal";
		if (q.lastSubmission)
			snprintf(submitted, sizeof(submitted), "last given work %llu ms before (%u list(s))",
				(unsigned long long)(now - q.lastSubmission), q.lastLists);
		if (q.lastSignal)
			snprintf(signalled, sizeof(signalled), "last asked to signal %llu ms before",
				(unsigned long long)(now - q.lastSignal));
		EchoXR_Log("  queue \"%s\"%s: %s, by %s; %s; %llu submission(s) of %llu list(s), %llu signal(s), %llu wait(s) "
			"since the start", q.name, q.queue == g_gameQueue ? " (the game's)" : "", submitted, by, signalled,
			(unsigned long long)q.submissions, (unsigned long long)q.lists, (unsigned long long)q.signals,
			(unsigned long long)q.waits);
	}
	if (queueCount == kQueues)
		EchoXR_Log("  queues past the first %d aren't counted", kQueues);

	// At max: the last signal of each of the last fences asked of a queue here, oldest first.
	if (max)
		for (const Signalled& r : ring)
			if (r.fence)
			{
				char by[128];
				EchoXR_Where(r.caller, by, sizeof(by));
				// The fence isn't referenced: only its address is printed.
				EchoXR_Log("  signal: fence %p to %llu on \"%s\", by %s", (void*)r.fence, (unsigned long long)r.value,
					r.queueName, by);
			}
}

void EchoXR_QueueSecond(char* out, size_t size)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (!g_on.load())
	{
		snprintf(out, size, "GPU waits not watched");
		return;
	}
	const ULONGLONG now = GetTickCount64();
	ULONGLONG oldest = 0;
	for (int i = 0; i < g_pendingCount; ++i)
		oldest = std::max<ULONGLONG>(oldest, now - g_pending[i].since);
	int used = snprintf(out, size, "GPU waits pending %d (the oldest %llu ms)", g_pendingCount, (unsigned long long)oldest);
	for (int i = 0; i < g_queueCount && used > 0 && (size_t)used < size; ++i)
	{
		QueueStats& q = g_queues[i];
		used += snprintf(out + used, size - used, ", \"%s\" %llu submission(s) %llu signal(s) %llu wait(s)", q.name,
			(unsigned long long)q.secondSubmissions, (unsigned long long)q.secondSignals,
			(unsigned long long)q.secondWaits);
		q.secondSubmissions = q.secondSignals = q.secondWaits = 0;
	}
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
	g_queueCount = 0;
	if (g_device)
		g_device->Release();
	g_device = nullptr;
	g_gameQueue = nullptr;
}
