// The device-removal watch (xr/src/diagnostics.cpp) on a real D3D12 device: Microsoft's WARP
// adapter on CI's Windows runner, the first GPU elsewhere. The device is removed on purpose
// (ID3D12Device5::RemoveDevice), and runtime.log's lines have to name it, once. A system with
// no D3D12 device, or one that can't remove a device (Wine's vkd3d), skips what it can't do;
// on WARP nothing may be skipped, so CI's pass means the removal was seen. CI runs it twice, the
// second time with ECHOXR_VERBOSE=max: then a line a second comes too, and only then.
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

static int g_failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

// DebugBreak, not __debugbreak: clang-cl's __try only covers exceptions raised in calls.
static void Breakpoint()
{
	__try
	{
		DebugBreak();
	}
	__except (GetExceptionCode() == EXCEPTION_BREAKPOINT ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
	{
	}
}

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	EchoXR_EnableDred();

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

	// The adapter line, with and without the runtime's adapter.
	const LUID own = device->GetAdapterLuid();
	LUID other = own;
	other.LowPart ^= 0x5a5a;
	EchoXR_WatchD3D12Device(device.Get(), other);
	CHECK(Count("NOT THE SAME ADAPTER") == 1);
	EchoXR_WatchD3D12Device(device.Get(), own);
	CHECK(Count("D3D12 device on ") == 2 && Count("NOT THE SAME ADAPTER") == 1);
	// Every adapter once, with its driver; the game adapter's video memory (WARP has some).
	CHECK(Count("Adapter 0: ") == 1 && Count(", driver ") >= 1);
	if (onWarp)
		CHECK(Count("Video memory of the game's adapter: ") == 1);
	CHECK(Count("Video memory") == 1);
	CHECK(Count("Watching the graphics device's allocations") == 1);

	// The line a second: only at max.
	const bool max = EchoXR_Max();
	CHECK(Count("Logging") == 0);   // xr_main.cpp's, not here
	for (int i = 0; i < 50 && Count("Second 1:") == 0; ++i)
		Sleep(50);
	CHECK(Count("Second 1:") == (max ? 1 : 0));
	if (max)
		CHECK(Count("xrWaitFrame 0, ") == 1 && Count("Command line: ") == 1);
	else
		CHECK(Count("Command line: ") == 0);

	// A breakpoint while the device is fine says nothing.
	Breakpoint();
	CHECK(Count("was removed") == 0);

	ComPtr<ID3D12Device5> device5;
	if (FAILED(device.As(&device5)))
	{
		std::printf("skipped the removal: no ID3D12Device5 here\n");
		CHECK(!onWarp);
		EchoXR_UnwatchDevice();
		return g_failures ? 1 : 0;
	}
	device5->RemoveDevice();
	if (device->GetDeviceRemovedReason() == S_OK)
	{
		std::printf("skipped the removal: RemoveDevice did nothing here\n");
		CHECK(!onWarp);
		EchoXR_UnwatchDevice();
		return g_failures ? 1 : 0;
	}

	// The watch hears of it by itself, as when the GPU hangs while the game waits elsewhere.
	for (int i = 0; i < 100 && Count("was removed") == 0; ++i)
		Sleep(50);
	CHECK(Count("was removed") == 1);
	CHECK(Count("noticed by EchoXR's watch") == 1);
	CHECK(Count("DRED") >= 1);
	CHECK(Count("Allocations since the start: ") == 1);
	if (onWarp)
		CHECK(Count("Video memory at the removal: ") == 1);
	// No more lines a second after the removal.
	const int seconds = Count("Second ");
	Sleep(1500);
	CHECK(Count("Second ") == seconds);

	// Echo VR's own int3 afterwards doesn't log it a second time.
	Breakpoint();
	CHECK(Count("was removed") == 1);

	EchoXR_UnwatchDevice();
	EchoXR_UnwatchDevice();   // a second session end is harmless
	if (g_failures == 0)
		std::printf("all checks passed\n");
	return g_failures ? 1 : 0;
}
