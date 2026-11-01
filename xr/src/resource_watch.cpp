// Resources and heaps made on D3D12 devices here (see echoxr_diag.h). Around the hangs on
// SteamVR's D3D12 path Echo VR logged "Failed to allocate resource" for players' cosmetics, and
// nothing said what failed or how much the game had made by then. These lines do.
#include "echoxr_diag.h"

#include <d3d12.h>
#include <detours/detours.h>
#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <mutex>

void EchoXR_Log(const char* fmt, ...);

namespace {

using CommittedCall = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
	const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using HeapCall = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_DESC*, REFIID, void**);
using PlacedCall = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
	D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using ReservedCall = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
	const D3D12_CLEAR_VALUE*, REFIID, void**);
using ResidencyCall = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, ID3D12Pageable* const*);
using Committed1Call = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
	const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*,
	REFIID, void**);
using Heap1Call = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_DESC*, ID3D12ProtectedResourceSession*,
	REFIID, void**);
// D3D12_RESOURCE_DESC1 starts with D3D12_RESOURCE_DESC's fields: read as one here.
using Committed2Call = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
	const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*,
	REFIID, void**);
using Placed1Call = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
	D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);

// ID3D12Device's vtable: ID3D12Device, then ID3D12Device4's and ID3D12Device8's additions.
constexpr UINT kCommittedSlot = 27, kHeapSlot = 28, kPlacedSlot = 29, kReservedSlot = 30;
constexpr UINT kMakeResidentSlot = 34, kEvictSlot = 35;
constexpr UINT kCommitted1Slot = 53, kHeap1Slot = 54;
constexpr UINT kCommitted2Slot = 69, kPlaced1Slot = 70;

CommittedCall TrueCommitted = nullptr;
HeapCall TrueHeap = nullptr;
PlacedCall TruePlaced = nullptr;
ReservedCall TrueReserved = nullptr;
ResidencyCall TrueMakeResident = nullptr;
ResidencyCall TrueEvict = nullptr;
Committed1Call TrueCommitted1 = nullptr;
Heap1Call TrueHeap1 = nullptr;
Committed2Call TrueCommitted2 = nullptr;
Placed1Call TruePlaced1 = nullptr;

constexpr UINT64 kBig = 8ull << 20;     // at max, every allocation this big gets a line
constexpr uint64_t kRemovedShown = 3;   // failures because the device is gone: the first few

// What was made: since the watch began, and in this second.
struct Counts
{
	uint64_t committed, committedBytes, placed, heaps, heapBytes, reserved, failed, resident, evicted;
};

std::mutex g_lock;
Counts g_total = {}, g_second = {};
ULONGLONG g_lastFailure = 0;
uint64_t g_removedFailures = 0;
bool g_hooked = false;
std::atomic<bool> g_on{ false };
// Inside a hook on this thread: a runtime that makes one kind through another is counted once.
thread_local int t_depth = 0;

double Mb(UINT64 bytes) { return bytes / (1024.0 * 1024.0); }

const char* HrName(HRESULT hr)
{
	switch (hr)
	{
	case E_OUTOFMEMORY: return "E_OUTOFMEMORY, out of video memory";
	case E_INVALIDARG: return "E_INVALIDARG";
	case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED, the device is gone";
	case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
	default: return "another error";
	}
}

const char* HeapName(const D3D12_HEAP_PROPERTIES* p)
{
	if (!p)
		return "no heap";
	switch (p->Type)
	{
	case D3D12_HEAP_TYPE_DEFAULT: return "default heap";
	case D3D12_HEAP_TYPE_UPLOAD: return "upload heap";
	case D3D12_HEAP_TYPE_READBACK: return "readback heap";
	case D3D12_HEAP_TYPE_CUSTOM: return "custom heap";
	default: return "another heap";
	}
}

// The bytes `desc` takes on `device`, or 0 when the device can't say (a request too big).
UINT64 SizeOf(ID3D12Device* device, const D3D12_RESOURCE_DESC* desc)
{
	if (!desc)
		return 0;
	if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		return desc->Width;
	const D3D12_RESOURCE_ALLOCATION_INFO info = device->GetResourceAllocationInfo(0, 1, desc);
	return info.SizeInBytes == UINT64_MAX ? 0 : info.SizeInBytes;
}

// "a buffer of 12.0 MB", "a 2048x2048 texture (format 71, 12 mip(s), 1 layer(s)), 22.4 MB".
void DescribeResource(const D3D12_RESOURCE_DESC* d, UINT64 bytes, char* out, size_t size)
{
	char amount[48];
	if (bytes)
		snprintf(amount, sizeof(amount), "%.1f MB", Mb(bytes));
	else
		snprintf(amount, sizeof(amount), "a size the device couldn't give");
	if (!d)
		snprintf(out, size, "no description");
	else if (d->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		snprintf(out, size, "a buffer of %s", amount);
	else
		snprintf(out, size, "a %llux%u texture (format %d, %u mip(s), %u %s), %s", (unsigned long long)d->Width,
			d->Height, (int)d->Format, d->MipLevels, d->DepthOrArraySize,
			d->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? "deep" : "layer(s)", amount);
}

// After a create call: counts it, and logs a failure (and at max one that is big).
void Made(ID3D12Device* device, const char* kind, const D3D12_RESOURCE_DESC* desc, const D3D12_HEAP_DESC* heap,
	const D3D12_HEAP_PROPERTIES* props, HRESULT hr, void* caller)
{
	const bool max = EchoXR_Max();
	const bool failed = FAILED(hr);
	const bool committed = strcmp(kind, "committed resource") == 0;
	const bool placed = strcmp(kind, "placed resource") == 0;
	// Placed resources take their heap's memory, reserved ones none until mapped: their size
	// only for a line.
	const UINT64 bytes = heap ? heap->SizeInBytes : (committed || failed || (max && placed)) ? SizeOf(device, desc) : 0;

	uint64_t failures = 0;
	bool log = false;
	{
		std::lock_guard<std::mutex> lock(g_lock);
		if (!g_on.load())
			return;
		Counts* counts[] = { &g_total, &g_second };
		for (Counts* c : counts)
		{
			if (failed)
				++c->failed;
			else if (heap)
			{
				++c->heaps;
				c->heapBytes += bytes;
			}
			else if (committed)
			{
				++c->committed;
				c->committedBytes += bytes;
			}
			else if (placed)
				++c->placed;
			else
				++c->reserved;
		}
		if (failed)
		{
			g_lastFailure = GetTickCount64();
			failures = g_total.failed;
			// Once the device is gone every call fails: the first few say so.
			log = hr == DXGI_ERROR_DEVICE_REMOVED ? ++g_removedFailures <= kRemovedShown : EchoXR_LogNth(failures);
		}
		else
			log = max && bytes >= kBig;
	}
	if (!log)
		return;

	char what[160], by[128];
	if (heap)
		snprintf(what, sizeof(what), "%.1f MB (%s)", Mb(heap->SizeInBytes), HeapName(&heap->Properties));
	else
	{
		DescribeResource(desc, bytes, what, sizeof(what));
		if (props)
		{
			const size_t used = strlen(what);
			snprintf(what + used, sizeof(what) - used, ", in the %s", HeapName(props));
		}
	}
	EchoXR_Where(caller, by, sizeof(by));
	if (failed)
		EchoXR_Log("Couldn't make a %s: 0x%08lX (%s); %s, asked by %s (%llu failed so far)", kind, (unsigned long)hr,
			HrName(hr), what, by, (unsigned long long)failures);
	else
		EchoXR_Log("Made a %s: %s, asked by %s", kind, what, by);
}

// The hooks: each passes the call on, then counts it. A call made from within another (the
// runtime making one kind through another) only passes on.
struct Depth
{
	const bool outer = t_depth++ == 0;
	~Depth() { --t_depth; }
};

HRESULT STDMETHODCALLTYPE HookCommitted(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* p, D3D12_HEAP_FLAGS f,
	const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID id, void** out)
{
	Depth depth;
	const HRESULT hr = TrueCommitted(d, p, f, desc, s, c, id, out);
	if (depth.outer && g_on.load(std::memory_order_relaxed))
		Made(d, "committed resource", desc, nullptr, p, hr, _ReturnAddress());
	return hr;
}

HRESULT STDMETHODCALLTYPE HookCommitted1(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* p, D3D12_HEAP_FLAGS f,
	const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c,
	ID3D12ProtectedResourceSession* session, REFIID id, void** out)
{
	Depth depth;
	const HRESULT hr = TrueCommitted1(d, p, f, desc, s, c, session, id, out);
	if (depth.outer && g_on.load(std::memory_order_relaxed))
		Made(d, "committed resource", desc, nullptr, p, hr, _ReturnAddress());
	return hr;
}

HRESULT STDMETHODCALLTYPE HookCommitted2(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* p, D3D12_HEAP_FLAGS f,
	const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c,
	ID3D12ProtectedResourceSession* session, REFIID id, void** out)
{
	Depth depth;
	const HRESULT hr = TrueCommitted2(d, p, f, desc, s, c, session, id, out);
	if (depth.outer && g_on.load(std::memory_order_relaxed))
		Made(d, "committed resource", (const D3D12_RESOURCE_DESC*)desc, nullptr, p, hr, _ReturnAddress());
	return hr;
}

HRESULT STDMETHODCALLTYPE HookHeap(ID3D12Device* d, const D3D12_HEAP_DESC* desc, REFIID id, void** out)
{
	Depth depth;
	const HRESULT hr = TrueHeap(d, desc, id, out);
	if (depth.outer && desc && g_on.load(std::memory_order_relaxed))
		Made(d, "heap", nullptr, desc, nullptr, hr, _ReturnAddress());
	return hr;
}

HRESULT STDMETHODCALLTYPE HookHeap1(ID3D12Device* d, const D3D12_HEAP_DESC* desc, ID3D12ProtectedResourceSession* session,
	REFIID id, void** out)
{
	Depth depth;
	const HRESULT hr = TrueHeap1(d, desc, session, id, out);
	if (depth.outer && desc && g_on.load(std::memory_order_relaxed))
		Made(d, "heap", nullptr, desc, nullptr, hr, _ReturnAddress());
	return hr;
}

HRESULT STDMETHODCALLTYPE HookPlaced(ID3D12Device* d, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc,
	D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID id, void** out)
{
	Depth depth;
	const HRESULT hr = TruePlaced(d, heap, offset, desc, s, c, id, out);
	if (depth.outer && g_on.load(std::memory_order_relaxed))
		Made(d, "placed resource", desc, nullptr, nullptr, hr, _ReturnAddress());
	return hr;
}

HRESULT STDMETHODCALLTYPE HookPlaced1(ID3D12Device* d, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC1* desc,
	D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID id, void** out)
{
	Depth depth;
	const HRESULT hr = TruePlaced1(d, heap, offset, desc, s, c, id, out);
	if (depth.outer && g_on.load(std::memory_order_relaxed))
		Made(d, "placed resource", (const D3D12_RESOURCE_DESC*)desc, nullptr, nullptr, hr, _ReturnAddress());
	return hr;
}

HRESULT STDMETHODCALLTYPE HookReserved(ID3D12Device* d, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES s,
	const D3D12_CLEAR_VALUE* c, REFIID id, void** out)
{
	Depth depth;
	const HRESULT hr = TrueReserved(d, desc, s, c, id, out);
	if (depth.outer && g_on.load(std::memory_order_relaxed))
		Made(d, "reserved resource", desc, nullptr, nullptr, hr, _ReturnAddress());
	return hr;
}

void Residency(uint64_t Counts::*field, UINT n)
{
	std::lock_guard<std::mutex> lock(g_lock);
	g_total.*field += n;
	g_second.*field += n;
}

HRESULT STDMETHODCALLTYPE HookMakeResident(ID3D12Device* d, UINT n, ID3D12Pageable* const* objects)
{
	if (g_on.load(std::memory_order_relaxed))
		Residency(&Counts::resident, n);
	return TrueMakeResident(d, n, objects);
}

HRESULT STDMETHODCALLTYPE HookEvict(ID3D12Device* d, UINT n, ID3D12Pageable* const* objects)
{
	if (g_on.load(std::memory_order_relaxed))
		Residency(&Counts::evicted, n);
	return TrueEvict(d, n, objects);
}

// Hooks `slot` of `vtable` once, unless another slot already led to the same method.
template <typename Call>
void Hook(void** vtable, UINT slot, Call& real, Call hook, void** done, int& doneCount)
{
	void* method = vtable[slot];
	for (int i = 0; i < doneCount; ++i)
		if (done[i] == method)
			return;
	done[doneCount++] = method;
	real = (Call)method;
	DetourAttach((PVOID*)&real, (PVOID)hook);
}

} // namespace

void EchoXR_WatchD3D12Resources(ID3D12Device* device)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (g_on.load() || !device)
		return;
	if (!g_hooked)
	{
		// On the driver's methods, as the queue watch does: every device here is seen. The hooks
		// stay for the process, idle while nothing is watched. Only the slots this device has.
		void** vtable = *(void***)device;
		ID3D12Device4* device4 = nullptr;
		ID3D12Device8* device8 = nullptr;
		const bool has4 = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device4)));
		const bool has8 = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device8)));
		void* done[10];
		int doneCount = 0;
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		Hook(vtable, kCommittedSlot, TrueCommitted, HookCommitted, done, doneCount);
		Hook(vtable, kHeapSlot, TrueHeap, HookHeap, done, doneCount);
		Hook(vtable, kPlacedSlot, TruePlaced, HookPlaced, done, doneCount);
		Hook(vtable, kReservedSlot, TrueReserved, HookReserved, done, doneCount);
		Hook(vtable, kMakeResidentSlot, TrueMakeResident, HookMakeResident, done, doneCount);
		Hook(vtable, kEvictSlot, TrueEvict, HookEvict, done, doneCount);
		if (has4)
		{
			void** vtable4 = *(void***)device4;
			Hook(vtable4, kCommitted1Slot, TrueCommitted1, HookCommitted1, done, doneCount);
			Hook(vtable4, kHeap1Slot, TrueHeap1, HookHeap1, done, doneCount);
		}
		if (has8)
		{
			void** vtable8 = *(void***)device8;
			Hook(vtable8, kCommitted2Slot, TrueCommitted2, HookCommitted2, done, doneCount);
			Hook(vtable8, kPlaced1Slot, TruePlaced1, HookPlaced1, done, doneCount);
		}
		const LONG result = DetourTransactionCommit();
		if (device4)
			device4->Release();
		if (device8)
			device8->Release();
		if (result != NO_ERROR)
		{
			EchoXR_Log("Couldn't watch the graphics device's allocations (its methods couldn't be hooked)");
			return;
		}
		g_hooked = true;
	}
	g_total = {};
	g_second = {};
	g_lastFailure = 0;
	g_removedFailures = 0;
	g_on.store(true);
	EchoXR_Log("Watching the graphics device's allocations%s", EchoXR_Max() ? " (every one of 8 MB and more gets a line)" : "");
}

void EchoXR_LogResources()
{
	Counts t;
	ULONGLONG lastFailure;
	{
		std::lock_guard<std::mutex> lock(g_lock);
		if (!g_on.load())
		{
			EchoXR_Log("  Allocations: not watched");
			return;
		}
		t = g_total;
		lastFailure = g_lastFailure;
	}
	char failures[96] = "none failed";
	if (t.failed)
		snprintf(failures, sizeof(failures), "%llu failed, the last %llu ms before", (unsigned long long)t.failed,
			(unsigned long long)(GetTickCount64() - lastFailure));
	EchoXR_Log("  Allocations since the start: %llu committed resource(s) of %.0f MB, %llu placed, %llu heap(s) of "
		"%.0f MB, %llu reserved; %s; %llu made resident, %llu evicted", (unsigned long long)t.committed,
		Mb(t.committedBytes), (unsigned long long)t.placed, (unsigned long long)t.heaps, Mb(t.heapBytes),
		(unsigned long long)t.reserved, failures, (unsigned long long)t.resident, (unsigned long long)t.evicted);
}

void EchoXR_ResourceSecond(char* out, size_t size)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (!g_on.load())
	{
		snprintf(out, size, "allocations not watched");
		return;
	}
	const Counts& c = g_second;
	snprintf(out, size, "made %llu committed resource(s) of %.1f MB, %llu placed, %llu heap(s) of %.1f MB, %llu reserved, "
		"%llu failed; %llu made resident, %llu evicted", (unsigned long long)c.committed, Mb(c.committedBytes),
		(unsigned long long)c.placed, (unsigned long long)c.heaps, Mb(c.heapBytes), (unsigned long long)c.reserved,
		(unsigned long long)c.failed, (unsigned long long)c.resident, (unsigned long long)c.evicted);
	g_second = {};
}

void EchoXR_UnwatchResources()
{
	std::lock_guard<std::mutex> lock(g_lock);
	g_on.store(false);
}
