/*
 * Minimal ImGui runtime for stripped DX12 hook build.
 */

#include "runtime_manager.hpp"
#include "dll_log.hpp"
#include "reshade_api_device.hpp"
#include "user_overlay.hpp"

#include "imgui.h"
#include "imgui_impl_dx12.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
	struct overlay_context
	{
		reshade::api::swapchain *swapchain_api = nullptr;
		IDXGISwapChain3 *swapchain = nullptr;
		ID3D12Device *device = nullptr;
		ID3D12CommandQueue *queue = nullptr;
		HWND hwnd = nullptr;

		ID3D12DescriptorHeap *rtv_heap = nullptr;
		ID3D12DescriptorHeap *srv_heap = nullptr;
		UINT rtv_descriptor_size = 0;
		UINT srv_descriptor_size = 0;
		UINT next_srv = 0;
		std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtv_handles;
		std::vector<ID3D12CommandAllocator *> command_allocators;
		std::vector<UINT64> frame_fence_values;

		ID3D12GraphicsCommandList *command_list = nullptr;
		ID3D12Fence *fence = nullptr;
		HANDLE fence_event = nullptr;
		UINT64 fence_value = 0;
		UINT buffer_count = 0;
		DXGI_FORMAT rtv_format = DXGI_FORMAT_UNKNOWN;

		bool initialized = false;
	};

	std::mutex s_mutex;
	std::unordered_map<reshade::api::swapchain *, overlay_context> s_contexts;
	ImGuiContext *s_imgui_ctx = nullptr;
	overlay_context *s_active_ctx = nullptr;
	bool s_imgui_backend_initialized = false;

	template <typename T>
	void safe_release(T *&obj)
	{
		if (obj != nullptr)
		{
			obj->Release();
			obj = nullptr;
		}
	}

	void clear_frame_resources(overlay_context &ctx)
	{
		for (ID3D12CommandAllocator *allocator : ctx.command_allocators)
			safe_release(allocator);
		ctx.command_allocators.clear();
		ctx.rtv_handles.clear();
		ctx.frame_fence_values.clear();
		safe_release(ctx.rtv_heap);
		ctx.buffer_count = 0;
	}

	void shutdown_imgui_backend()
	{
		if (!s_imgui_backend_initialized || s_imgui_ctx == nullptr)
			return;

		ImGui::SetCurrentContext(s_imgui_ctx);
		ImGui_ImplDX12_Shutdown();
		s_imgui_backend_initialized = false;
	}

	void setup_frame_resources(overlay_context &ctx)
	{
		clear_frame_resources(ctx);

		DXGI_SWAP_CHAIN_DESC desc = {};
		ctx.swapchain->GetDesc(&desc);
		ctx.buffer_count = desc.BufferCount > 0 ? desc.BufferCount : 2;
		ctx.rtv_format = desc.BufferDesc.Format;

		D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
		rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtv_desc.NumDescriptors = ctx.buffer_count;
		rtv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		ctx.device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&ctx.rtv_heap));
		ctx.rtv_descriptor_size = ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

		ctx.rtv_handles.resize(ctx.buffer_count);
		ctx.command_allocators.resize(ctx.buffer_count);
		ctx.frame_fence_values.assign(ctx.buffer_count, 0);

		D3D12_CPU_DESCRIPTOR_HANDLE rtv_base = ctx.rtv_heap->GetCPUDescriptorHandleForHeapStart();
		for (UINT i = 0; i < ctx.buffer_count; ++i)
		{
			ctx.rtv_handles[i].ptr = rtv_base.ptr + static_cast<SIZE_T>(i) * ctx.rtv_descriptor_size;
			ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ctx.command_allocators[i]));
		}
	}

	void srv_alloc(ImGui_ImplDX12_InitInfo *info, D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu_desc_handle)
	{
		auto *ctx = static_cast<overlay_context *>(info->UserData);
		const UINT index = ctx->next_srv++;
		const D3D12_CPU_DESCRIPTOR_HANDLE cpu_start = ctx->srv_heap->GetCPUDescriptorHandleForHeapStart();
		const D3D12_GPU_DESCRIPTOR_HANDLE gpu_start = ctx->srv_heap->GetGPUDescriptorHandleForHeapStart();
		out_cpu_desc_handle->ptr = cpu_start.ptr + static_cast<SIZE_T>(index) * ctx->srv_descriptor_size;
		out_gpu_desc_handle->ptr = gpu_start.ptr + static_cast<UINT64>(index) * ctx->srv_descriptor_size;
	}

	void srv_free(ImGui_ImplDX12_InitInfo *, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE)
	{
	}

	void ensure_imgui_initialized(overlay_context &ctx)
	{
		if (s_imgui_ctx == nullptr)
			s_imgui_ctx = ImGui::CreateContext();

		ImGui::SetCurrentContext(s_imgui_ctx);
		if (!s_imgui_backend_initialized)
		{
			if (ctx.srv_heap == nullptr)
			{
				D3D12_DESCRIPTOR_HEAP_DESC srv_desc = {};
				srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				srv_desc.NumDescriptors = 64;
				srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
				ctx.device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&ctx.srv_heap));
				ctx.srv_descriptor_size = ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				ctx.next_srv = 0;
			}

			ImGui_ImplDX12_InitInfo init_info = {};
			init_info.Device = ctx.device;
			init_info.CommandQueue = ctx.queue;
			init_info.NumFramesInFlight = static_cast<int>(ctx.buffer_count);
			init_info.RTVFormat = ctx.rtv_format;
			init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
			init_info.SrvDescriptorHeap = ctx.srv_heap;
			init_info.SrvDescriptorAllocFn = &srv_alloc;
			init_info.SrvDescriptorFreeFn = &srv_free;
			init_info.UserData = &ctx;

			s_active_ctx = &ctx;
			s_imgui_backend_initialized = ImGui_ImplDX12_Init(&init_info);
			if (s_imgui_backend_initialized)
				ImGui::StyleColorsDark();
			else
				reshade::log::message(reshade::log::level::error, "ImGui DX12 backend initialization failed.");
		}
	}
}

void reshade::create_effect_runtime(api::swapchain *swapchain, ID3D12CommandQueue *queue)
{
	std::lock_guard<std::mutex> lock(s_mutex);

	auto &ctx = s_contexts[swapchain];
	ctx.swapchain_api = swapchain;
	ctx.swapchain = reinterpret_cast<IDXGISwapChain3 *>(swapchain->get_native());
	ctx.queue = queue;
	ctx.hwnd = static_cast<HWND>(swapchain->get_hwnd());

	if (ctx.device == nullptr)
	{
		ctx.queue->GetDevice(IID_PPV_ARGS(&ctx.device));
	}
}

void reshade::init_effect_runtime(api::swapchain *swapchain)
{
	std::lock_guard<std::mutex> lock(s_mutex);
	const auto it = s_contexts.find(swapchain);
	if (it == s_contexts.end())
		return;

	overlay_context &ctx = it->second;
	if (ctx.device == nullptr || ctx.queue == nullptr || ctx.swapchain == nullptr)
		return;

	setup_frame_resources(ctx);
	ensure_imgui_initialized(ctx);

	if (ctx.command_list == nullptr)
	{
		ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ctx.command_allocators[0], nullptr, IID_PPV_ARGS(&ctx.command_list));
		ctx.command_list->Close();
	}
	if (ctx.fence == nullptr)
		ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ctx.fence));
	if (ctx.fence_event == nullptr)
		ctx.fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	ctx.initialized = s_imgui_backend_initialized;
}

void reshade::reset_effect_runtime(api::swapchain *swapchain)
{
	std::lock_guard<std::mutex> lock(s_mutex);
	const auto it = s_contexts.find(swapchain);
	if (it == s_contexts.end())
		return;

	overlay_context &ctx = it->second;
	ctx.initialized = false;

	shutdown_imgui_backend();
	clear_frame_resources(ctx);
}

void reshade::present_effect_runtime(api::swapchain *swapchain)
{
	std::lock_guard<std::mutex> lock(s_mutex);
	const auto it = s_contexts.find(swapchain);
	if (it == s_contexts.end())
		return;

	overlay_context &ctx = it->second;
	if (!ctx.initialized || !s_imgui_backend_initialized || ctx.command_list == nullptr || ctx.fence == nullptr)
		return;

	DXGI_SWAP_CHAIN_DESC desc = {};
	ctx.swapchain->GetDesc(&desc);
	if (desc.BufferCount != ctx.buffer_count && desc.BufferCount > 0)
	{
		ctx.initialized = false;
		shutdown_imgui_backend();
		setup_frame_resources(ctx);
		ensure_imgui_initialized(ctx);
		ctx.initialized = s_imgui_backend_initialized;
		if (!ctx.initialized)
			return;
	}

	ImGui::SetCurrentContext(s_imgui_ctx);
	ImGui_ImplDX12_NewFrame();

	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2(static_cast<float>(desc.BufferDesc.Width), static_cast<float>(desc.BufferDesc.Height));
	io.DeltaTime = 1.0f / 60.0f;

	ImGui::NewFrame();
	user_overlay::frame_context frame_ctx = {};
	frame_ctx.delta_time = io.DeltaTime;
	frame_ctx.width = desc.BufferDesc.Width;
	frame_ctx.height = desc.BufferDesc.Height;
	user_overlay::render(frame_ctx);
	ImGui::Render();

	const UINT frame_index = ctx.swapchain->GetCurrentBackBufferIndex();
	if (frame_index >= ctx.command_allocators.size())
		return;

	if (ctx.fence->GetCompletedValue() < ctx.frame_fence_values[frame_index])
	{
		ctx.fence->SetEventOnCompletion(ctx.frame_fence_values[frame_index], ctx.fence_event);
		WaitForSingleObject(ctx.fence_event, INFINITE);
	}

	ID3D12Resource *back_buffer = nullptr;
	if (FAILED(ctx.swapchain->GetBuffer(frame_index, IID_PPV_ARGS(&back_buffer))))
		return;

	ctx.device->CreateRenderTargetView(back_buffer, nullptr, ctx.rtv_handles[frame_index]);

	ctx.command_allocators[frame_index]->Reset();
	ctx.command_list->Reset(ctx.command_allocators[frame_index], nullptr);

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = back_buffer;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	ctx.command_list->ResourceBarrier(1, &barrier);

	ctx.command_list->OMSetRenderTargets(1, &ctx.rtv_handles[frame_index], FALSE, nullptr);
	ID3D12DescriptorHeap *heaps[] = { ctx.srv_heap };
	ctx.command_list->SetDescriptorHeaps(1, heaps);
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), ctx.command_list);

	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	ctx.command_list->ResourceBarrier(1, &barrier);

	ctx.command_list->Close();
	ID3D12CommandList *lists[] = { ctx.command_list };
	ctx.queue->ExecuteCommandLists(1, lists);

	ctx.fence_value++;
	ctx.queue->Signal(ctx.fence, ctx.fence_value);
	ctx.frame_fence_values[frame_index] = ctx.fence_value;

	back_buffer->Release();
}

void reshade::destroy_effect_runtime(api::swapchain *swapchain)
{
	std::lock_guard<std::mutex> lock(s_mutex);
	const auto it = s_contexts.find(swapchain);
	if (it == s_contexts.end())
		return;

	overlay_context &ctx = it->second;
	if (ctx.fence != nullptr && ctx.queue != nullptr)
	{
		ctx.fence_value++;
		ctx.queue->Signal(ctx.fence, ctx.fence_value);
		ctx.fence->SetEventOnCompletion(ctx.fence_value, ctx.fence_event);
		WaitForSingleObject(ctx.fence_event, INFINITE);
	}

	shutdown_imgui_backend();
	clear_frame_resources(ctx);

	if (ctx.fence_event != nullptr)
	{
		CloseHandle(ctx.fence_event);
		ctx.fence_event = nullptr;
	}
	safe_release(ctx.fence);
	safe_release(ctx.command_list);
	safe_release(ctx.srv_heap);
	safe_release(ctx.device);

	s_contexts.erase(it);
}

void reshade::create_effect_runtime(D3D12CommandQueueDownlevel *, D3D12CommandQueue *)
{
}
void reshade::init_effect_runtime(D3D12CommandQueueDownlevel *)
{
}
void reshade::reset_effect_runtime(D3D12CommandQueueDownlevel *)
{
}
void reshade::present_effect_runtime(D3D12CommandQueueDownlevel *)
{
}
void reshade::destroy_effect_runtime(D3D12CommandQueueDownlevel *)
{
}
