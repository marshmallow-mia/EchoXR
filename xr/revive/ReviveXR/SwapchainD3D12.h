#pragma once

#include "Swapchain.h"

struct ovrTextureSwapChainD3D12 : public ovrTextureSwapChainData
{
public:
	ovrTextureSwapChainD3D12(struct ID3D12CommandQueue* queue);
	~ovrTextureSwapChainD3D12();

	virtual ovrResult Commit(ovrSession session) override;

	ovrResult InitBarriers();
	void WaitForEarlierRelease();

	static ovrResult Create(ovrSession session, struct ID3D12CommandQueue* queue, const ovrTextureSwapChainDesc* desc, ovrTextureSwapChain* out_TextureSwapChain);

private:
	struct ID3D12CommandQueue* m_Queue;
	struct ID3D12Device* m_Device;
	struct ID3D12CommandAllocator* m_Allocator;
	struct ID3D12GraphicsCommandList** m_AcquireBarriers;
	struct ID3D12GraphicsCommandList** m_ReleaseBarriers;
	struct ID3D12Fence* m_Fence;     // signalled on the game's queue as each release begins
	void* m_Event;
	unsigned long long m_Signaled;   // the last value asked for
	unsigned long long m_LongWaits;
};
