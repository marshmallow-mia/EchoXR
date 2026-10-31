// What runtime.log says about a graphics device that stops responding. Echo VR on SteamVR's
// D3D12 path ended in DXGI_ERROR_DEVICE_HUNG 30-60 s into a start, and nothing in the log said
// which side the GPU was waiting for. These lines name it (xr/src/diagnostics.cpp).
#pragma once

#include <windows.h>

#include <cstdint>

struct ID3D12Device;

// Times one OpenXR frame call. The call's name is kept while it runs, so a device removal names
// the call in progress, and a call slower than 200 ms gets a line (the first ten, then every
// hundredth).
class EchoXR_Call
{
public:
	explicit EchoXR_Call(const char* name);
	~EchoXR_Call();
	EchoXR_Call(const EchoXR_Call&) = delete;
	EchoXR_Call& operator=(const EchoXR_Call&) = delete;

private:
	const char* m_Name;
	const char* m_Outer;
	ULONGLONG m_Start;
};

// A swapchain image wasn't ready within a frame (`gaveUp`: not within 5 s either).
void EchoXR_SwapchainWaitTimedOut(int64_t waitedNs, bool gaveUp);

// Asks D3D12 for DRED's breadcrumbs and page-fault data before the game creates its device.
// Echo VR turns them on itself; this covers a build that doesn't. Costs a little CPU per
// command list.
void EchoXR_EnableDred();

// Logs the game's adapter next to the one the OpenXR runtime asked for, and watches the device:
// once it is removed, runtime.log gets the reason, the command lists the GPU didn't finish and
// the operation it was on. Echo VR stops itself right after it sees a removal (an int3 in its
// error handler), so the log is also written from that breakpoint if the watch didn't run yet.
void EchoXR_WatchD3D12Device(ID3D12Device* device, LUID required);
void EchoXR_UnwatchDevice();
