#pragma once
#include <d3d12.h>

namespace reshade::api
{
	struct swapchain;
}

class D3D12CommandQueue;
class D3D12CommandQueueDownlevel;

namespace reshade
{
	void create_effect_runtime(api::swapchain *swapchain, ID3D12CommandQueue *queue);
	void init_effect_runtime(api::swapchain *swapchain);
	void reset_effect_runtime(api::swapchain *swapchain);
	void present_effect_runtime(api::swapchain *swapchain);
	void destroy_effect_runtime(api::swapchain *swapchain);

	// Keep d3d12on7 path inert in this minimal build.
	void create_effect_runtime(D3D12CommandQueueDownlevel *, D3D12CommandQueue *);
	void init_effect_runtime(D3D12CommandQueueDownlevel *);
	void reset_effect_runtime(D3D12CommandQueueDownlevel *);
	void present_effect_runtime(D3D12CommandQueueDownlevel *);
	void destroy_effect_runtime(D3D12CommandQueueDownlevel *);
}
