// What runtime.log says about a graphics device that stops responding. Echo VR on SteamVR's
// D3D12 path ended in DXGI_ERROR_DEVICE_HUNG 30-60 s into a start, and nothing in the log said
// which side the GPU was waiting for. These lines name it (xr/src/diagnostics.cpp).
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "echoxr_policy.h"

struct ID3D12CommandQueue;
struct ID3D12Device;

// How much runtime.log says: ECHOXR_VERBOSE, read once (EchoXR.exe --verbose sets it).
echoxr::Verbose EchoXR_Verbose();
inline bool EchoXR_Max() { return EchoXR_Verbose() == echoxr::Verbose::Max; }
// echoxr::LogNth, or every line at max.
bool EchoXR_LogNth(uint64_t count);
// Whether the line being written now is one runtime.log keeps past its size cap: a removal's
// report (xr_main.cpp's EchoXR_Log asks).
bool EchoXR_LogImportant();

// Times one OpenXR frame call. The call's name is kept while it runs, so a device removal names
// the call in progress, and a call slower than 200 ms (50 ms at max) gets a line (the first
// ten, then every hundredth; at max all). At max a line a second has their average and longest.
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
	LONGLONG m_Start;
};

// A swapchain image wasn't ready within a frame (`gaveUp`: not within 5 s either).
void EchoXR_SwapchainWaitTimedOut(int64_t waitedNs, bool gaveUp);

// Asks D3D12 for DRED's breadcrumbs and page-fault data before the game creates its device.
// Echo VR turns them on itself; this covers a build that doesn't. Costs a little CPU per
// command list.
void EchoXR_EnableDred();

// ECHOXR_D3D12_DEBUG=1 turns on the D3D12 debug layer before the game creates its device, =gbv
// also GPU-based validation and synchronized queue validation (slow). Its warnings and errors
// go to runtime.log once a second (the first three of each message ID, then counted; a count
// per ID every minute and at a removal). Needs Windows' optional feature "Graphics Tools".
void EchoXR_EnableDebugLayer();

// Logs the game's adapter next to the one the OpenXR runtime asked for, and watches the device:
// once it is removed, runtime.log gets the reason, the command lists the GPU didn't finish and
// the operation it was on. Echo VR stops itself right after it sees a removal (an int3 in its
// error handler), so the log is also written from that breakpoint if the watch didn't run yet.
// The first watch also logs every adapter (memory, driver version) and the video memory the
// game's may use; a line when that budget changes and when its use passes 90% and 100% of it,
// and at max a line a second with what every watch here counted.
void EchoXR_WatchD3D12Device(ID3D12Device* device, LUID required);
void EchoXR_UnwatchDevice();

// Watches the GPU's waits on the game's queue `queue` and on every other D3D12 queue in the
// process (the hook sits on the driver's methods, so SteamVR's are seen too). A wait still
// pending after half a second gets a line: the queue, the fence, the value it waits for, the
// module that queued it and the last signal of that fence asked of a queue here. A removal's
// report lists the waits pending then (xr/src/queue_watch.cpp). The watch ends with
// EchoXR_UnwatchDevice.
void EchoXR_WatchD3D12Queue(ID3D12CommandQueue* queue);
// For the removal report and EchoXR_UnwatchDevice. The report also says, per queue, when it
// was last given work and asked for a signal.
void EchoXR_LogQueueWaits();
void EchoXR_UnwatchQueues();
// The queue watch's part of the line a second: the pending waits and, per queue, the work and
// signals of the second (counted from the last call).
void EchoXR_QueueSecond(char* out, size_t size);

// Watches the resources and heaps made on D3D12 devices here (xr/src/resource_watch.cpp): a
// failed one gets a line (its size, kind, the module that asked), at max every one of 8 MB and
// more. The removal report has the totals. The watch ends with EchoXR_UnwatchDevice.
void EchoXR_WatchD3D12Resources(ID3D12Device* device);
void EchoXR_LogResources();
void EchoXR_UnwatchResources();
// The resource watch's part of the line a second (counted from the last call).
void EchoXR_ResourceSecond(char* out, size_t size);

// The module and offset of `address`, such as "vrclient_x64.dll+0x1a2b3" (takes the loader lock).
void EchoXR_Where(void* address, char* out, size_t size);
