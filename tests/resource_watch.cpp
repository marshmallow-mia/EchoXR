// The allocation watch (xr/src/resource_watch.cpp) on a real D3D12 device: Microsoft's WARP
// adapter on CI's Windows runner, the first GPU elsewhere. A buffer no device makes (one of two
// rows: invalid, so nothing is allocated; a request too big crashes Wine's Vulkan driver) has to
// get a line naming it, its size and this program as the one that asked; one that can be made
// gets none, unless it is 8 MB or more and ECHOXR_VERBOSE=max (CI runs this twice). The removal
// report has the totals.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "echoxr_diag.h"

#include <cstdarg>
#include <cstdio>
#include <initializer_list>
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

static int g_failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static D3D12_RESOURCE_DESC Buffer(UINT64 bytes)
{
	D3D12_RESOURCE_DESC d = {};
	d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	d.Width = bytes;
	d.Height = 1;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.SampleDesc.Count = 1;
	d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	return d;
}

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);

	ComPtr<IDXGIFactory4> factory;
	ComPtr<IDXGIAdapter> warp;
	ComPtr<ID3D12Device> device;
	if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))))
		D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
	if (!device && FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
	{
		std::printf("skipped: no D3D12 device here\n");
		return 0;
	}
	const bool max = EchoXR_Max();

	EchoXR_WatchD3D12Device(device.Get(), device->GetAdapterLuid());
	CHECK(CountAll({ "Watching the graphics device's allocations" }) == 1);

	const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
	D3D12_RESOURCE_DESC bad = Buffer(1 << 20);
	bad.Height = 2;
	ComPtr<ID3D12Resource> r;
	const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bad, D3D12_RESOURCE_STATE_COMMON,
		nullptr, IID_PPV_ARGS(&r));
	CHECK(FAILED(hr));
	CHECK(CountAll({ "Couldn't make a committed resource: 0x", "a buffer of 1.0 MB", "in the default heap",
		"resource_watch.exe+0x", "(1 failed so far)" }) == 1);

	// 16 MB can be made: a line only at max.
	const D3D12_RESOURCE_DESC big = Buffer(16ull << 20);
	CHECK(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &big, D3D12_RESOURCE_STATE_COMMON,
		nullptr, IID_PPV_ARGS(&r))));
	CHECK(CountAll({ "Made a committed resource: a buffer of 16.0 MB", "resource_watch.exe+0x" }) == (max ? 1 : 0));
	// 64 KB never.
	const D3D12_RESOURCE_DESC little = Buffer(64 << 10);
	ComPtr<ID3D12Resource> s;
	CHECK(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &little, D3D12_RESOURCE_STATE_COMMON,
		nullptr, IID_PPV_ARGS(&s))));
	CHECK(CountAll({ "Made a committed resource: a buffer of 0.1 MB" }) == 0);

	// A heap of nothing.
	D3D12_HEAP_DESC hd = {};
	hd.SizeInBytes = 0;
	hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
	hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
	ComPtr<ID3D12Heap> h;
	CHECK(FAILED(device->CreateHeap(&hd, IID_PPV_ARGS(&h))));
	CHECK(CountAll({ "Couldn't make a heap: 0x", "0.0 MB in a default heap", "(2 failed so far)" }) == 1);

	// At max the line a second has the second's allocations.
	if (max)
	{
		for (int i = 0; i < 50 && CountAll({ "Second 1:" }) == 0; ++i)
			Sleep(50);
		CHECK(CountAll({ "Second 1:", "made 2 committed resource(s) of 16.1 MB", "2 failed" }) == 1);
	}

	// The totals, as a removal's report has them.
	EchoXR_LogResources();
	CHECK(CountAll({ "Allocations since the start: 2 committed resource(s) of 16 MB", "2 failed, the last " }) == 1);

	EchoXR_UnwatchDevice();
	// Unwatched: nothing more.
	ComPtr<ID3D12Resource> after;
	device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bad, D3D12_RESOURCE_STATE_COMMON, nullptr,
		IID_PPV_ARGS(&after));
	CHECK(CountAll({ "Couldn't make a committed resource" }) == 1);
	if (g_failures == 0)
		std::printf("all checks passed\n");
	return g_failures ? 1 : 0;
}
