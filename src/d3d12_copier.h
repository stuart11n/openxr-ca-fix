#pragma once

#include <d3d12.h>

#include <cstdint>
#include <vector>

// Records GPU copies from app swapchain images into the layer's swapchain
// images and submits them on the app's queue, so they run after the app's own
// rendering for that frame. Swapchain images are in RENDER_TARGET state when
// released (XR_KHR_D3D12_enable), so that is the state we return them in.
class D3D12Copier {
public:
    D3D12Copier() = default;
    ~D3D12Copier();
    D3D12Copier(const D3D12Copier&) = delete;
    D3D12Copier& operator=(const D3D12Copier&) = delete;

    bool Init(ID3D12Device* device, ID3D12CommandQueue* queue);
    bool Ready() const { return list_ != nullptr; }

    void BeginFrame();
    void AddCopy(ID3D12Resource* src, ID3D12Resource* dst);
    bool EndFrame();  // closes and submits if anything was recorded
    void WaitIdle();

private:
    void WaitFor(uint64_t value);

    static constexpr int kFrames = 3;
    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    ID3D12CommandAllocator* allocators_[kFrames] = {};
    uint64_t allocatorFence_[kFrames] = {};
    ID3D12GraphicsCommandList* list_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    HANDLE event_ = nullptr;
    uint64_t fenceValue_ = 0;
    int slot_ = 0;
    bool recording_ = false;
    int copies_ = 0;
};
