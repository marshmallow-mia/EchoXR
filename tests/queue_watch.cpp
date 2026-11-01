// The GPU-wait watch (xr/src/queue_watch.cpp) on a real D3D12 queue: Microsoft's WARP adapter
// on CI's Windows runner, the first GPU elsewhere. The queue waits for a fence nobody has
// signalled yet: after half a second runtime.log names the wait, its queue and this program
// as the caller, and once the fence gets there it is gone. A removed device's report lists the
// waits pending then, with the value the fence had and the last signal another queue was
// asked for, and per queue the work it was given. A system that can't remove a device (Wine's
// vkd3d) skips that part; on WARP nothing may be skipped. CI runs it again with
// ECHOXR_VERBOSE=max: the line a second counts the queues' work, and the report lists the
// signals too.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "echoxr_diag.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static std::mutex g_linesLock;
static std::vector<std::string> g_lines;

// Stands in for xr_main.cpp's runtime.log.
void EchoXR_Log(const char* fmt, ...)
{
	char line[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	std::printf("  log: %s\n", line);
	std::lock_guard<std::mutex> lock(g_linesLock);
	g_lines.push_back(line);
}

static int Count(const char* text)
{
	std::lock_guard<std::mutex> lock(g_linesLock);
	int n = 0;
	for (const std::string& l : g_lines)
		n += l.find(text) != std::string::npos;
	return n;
}

// Lines that have every one of `parts`.
static int CountAll(std::initializer_list<const char*> parts)
{
	std::lock_guard<std::mutex> lock(g_linesLock);
	int n = 0;
	for (const std::string& l : g_lines)
	{
		bool all = true;
		for (const char* p : parts)
			all = all && l.find(p) != std::string::npos;
		n += all;
	}
	return n;
}

static void WaitFor(const char* text, int count, int ms)
{
	for (int waited = 0; waited < ms && Count(text) < count; waited += 50)
		Sleep(50);
}

static int g_failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

// Without a removal: the game's queue gets its fence, so it is idle when it goes.
static int Finish(ID3D12Fence* held, UINT64 value)
{
	held->Signal(value);
	EchoXR_UnwatchDevice();
	if (g_failures == 0)
		std::printf("all checks passed\n");
	return g_failures ? 1 : 0;
}

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);

	ComPtr<IDXGIFactory4> factory;
	ComPtr<IDXGIAdapter> warp;
	ComPtr<ID3D12Device> device;
	if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))))
		D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
	// Wine hands out a real GPU as its "WARP" adapter: only Microsoft's own counts.
	DXGI_ADAPTER_DESC desc = {};
	const bool onWarp = device && SUCCEEDED(warp->GetDesc(&desc)) && desc.VendorId == 0x1414 && desc.DeviceId == 0x8c;
	if (!device && FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
	{
		std::printf("skipped: no D3D12 device here\n");
		return 0;
	}

	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	ComPtr<ID3D12CommandQueue> game, other;
	ComPtr<ID3D12Fence> a, b;
	if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&game))) ||
		FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&other))) ||
		FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&a))) ||
		FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&b))))
	{
		std::printf("FAIL: no queues or fences on this device\n");
		return 1;
	}
	game->SetName(L"Game Queue");
	other->SetName(L"Other Queue");
	const bool max = EchoXR_Max();

	EchoXR_WatchD3D12Device(device.Get(), device->GetAdapterLuid());
	EchoXR_WatchD3D12Queue(game.Get());
	CHECK(Count("Watching the GPU's waits") == 1);
	EchoXR_WatchD3D12Queue(game.Get());   // a second watch is ignored
	CHECK(Count("Watching the GPU's waits") == 1);

	// A wait for a value the fence already has holds nothing.
	a->Signal(1);
	game->Wait(a.Get(), 1);
	// One it doesn't have yet: named after half a second, with this program as the caller.
	game->Wait(a.Get(), 2);
	WaitFor("still pending", 1, 3000);
	CHECK(Count("still pending") == 1);
	CHECK(CountAll({ "queue \"Game Queue\" (the game's)", "to reach 2 (it was at 1)", "queue_watch.exe+0x",
		"no queue here was asked to signal it" }) == 1);

	// Once the fence gets there the wait is over: no more lines about it.
	a->Signal(2);
	Sleep(300);
	game->Wait(a.Get(), 2);
	Sleep(800);
	CHECK(Count("still pending") == 1);

	// Another queue signals b to 3; the game's waits for 7. The line has both.
	HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	other->Signal(b.Get(), 3);
	b->SetEventOnCompletion(3, done);
	WaitForSingleObject(done, 3000);
	CloseHandle(done);
	game->Wait(b.Get(), 7);
	WaitFor("still pending", 2, 3000);
	CHECK(CountAll({ "still pending", "to reach 7 (it was at 3)",
		"the last signal of that fence asked of a queue here: 3 on \"Other Queue\", by queue_watch.exe+0x" }) == 1);

	// Some work for the game's queue: an empty command list.
	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
		SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) &&
		SUCCEEDED(list->Close()))
	{
		ID3D12CommandList* lists[] = { list.Get() };
		game->ExecuteCommandLists(1, lists);
	}
	else
		CHECK(!"no command list");

	// At max the line a second has the queues' counts (once the other queue was used, the next
	// line has it too); otherwise there is none.
	for (int waited = 0; waited < 2500 && CountAll({ "Second ", "GPU waits pending ", "\"Other Queue\" " }) == 0; waited += 50)
		Sleep(50);
	CHECK((Count("Second ") > 0) == max);
	if (max)
		CHECK(CountAll({ "Second ", "GPU waits pending ", "\"Other Queue\" " }) >= 1);

	ComPtr<ID3D12Device5> device5;
	if (FAILED(device.As(&device5)))
	{
		std::printf("skipped the removal: no ID3D12Device5 here\n");
		CHECK(!onWarp);
		return Finish(b.Get(), 7);
	}
	device5->RemoveDevice();
	if (device->GetDeviceRemovedReason() == S_OK)
	{
		std::printf("skipped the removal: RemoveDevice did nothing here\n");
		CHECK(!onWarp);
		return Finish(b.Get(), 7);
	}

	// The removal's report lists the wait still pending, as it was before the removal.
	// Its last line is the allocations'.
	WaitFor("Allocations since the start", 1, 5000);
	CHECK(Count("was removed") == 1);
	CHECK(Count("GPU waits pending at the removal: 1 (oldest first)") == 1);
	CHECK(CountAll({ "    queue \"Game Queue\" (the game's)", "to reach 7 (it was at 3)" }) == 1);
	// Per queue: the game's was given the list, the other only asked to signal.
	CHECK(CountAll({ "  queue \"Game Queue\" (the game's): last given work ", "(1 list(s))",
		"queue_watch.exe+0x", "1 submission(s) of 1 list(s), 0 signal(s), 4 wait(s)" }) == 1);
	CHECK(CountAll({ "  queue \"Other Queue\": never given work, by nobody; last asked to signal ",
		"0 submission(s) of 0 list(s), 1 signal(s), 0 wait(s)" }) == 1);
	CHECK(CountAll({ "  signal: fence ", "to 3 on \"Other Queue\"" }) == (max ? 1 : 0));

	const int lines = Count("");
	EchoXR_UnwatchDevice();
	EchoXR_UnwatchDevice();   // a second session end is harmless
	CHECK(Count("") == lines);
	if (g_failures == 0)
		std::printf("all checks passed\n");
	return g_failures ? 1 : 0;
}
