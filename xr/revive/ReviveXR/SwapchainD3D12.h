#pragma once

#include "Swapchain.h"

#include <vector>

struct ovrTextureSwapChainD3D12 : public ovrTextureSwapChainData
{
public:
	ovrTextureSwapChainD3D12(struct ID3D12CommandQueue* queue);
	~ovrTextureSwapChainD3D12();

	virtual ovrResult Commit(ovrSession session) override;

	ovrResult InitBarriers();
	void WaitForEarlierRelease();
	void SignalRelease();

	static ovrResult Create(ovrSession session, struct ID3D12CommandQueue* queue, const ovrTextureSwapChainDesc* desc, ovrTextureSwapChain* out_TextureSwapChain);

private:
	struct ID3D12CommandQueue* m_Queue;
	struct ID3D12Device* m_Device;
	struct ID3D12CommandAllocator* m_Allocator;
	struct ID3D12GraphicsCommandList** m_AcquireBarriers;
	struct ID3D12GraphicsCommandList** m_ReleaseBarriers;
	struct ID3D12GraphicsCommandList** m_FirstBarriers;   // depth: an image's first hand-over (to COMMON)
	std::vector<struct ID3D12GraphicsCommandList*> m_FirstUsed;
	struct ID3D12Fence* m_Fence;     // signalled on the game's queue after each release
	void* m_Event;
	unsigned long long m_Signaled;   // the last value asked for
	unsigned long long m_LongWaits;
};
