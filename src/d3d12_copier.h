#pragma once

#include <d3d12.h>
#include <dxgiformat.h>

#include <cstdint>
#include <unordered_map>

// Records GPU work that moves app swapchain images into the layer's swapchain
// images and submits it on the app's queue, so it runs after the app's own
// rendering for that frame. Swapchain images are in RENDER_TARGET state when
// released (XR_KHR_D3D12_enable), so that is the state we return them in.
//
// Two ways to move an image: a plain CopyResource, or a full-screen pass that
// resamples R and B radially around the lens centre (lateral CA correction).
class D3D12Copier {
public:
    // Matches the cbuffer in the pixel shader (root constants, float4-aligned).
    struct CaParams {
        float tanLeft, tanRight, tanUp, tanDown;  // view frustum, tangent units
        float centerX, centerY;                   // lens centre, tangent units
        float redK1, redK2;
        float blueK1, blueK2;
        float uvOffsetX, uvOffsetY;               // image rect within the texture, in UV
        float uvScaleX, uvScaleY;
        float rectWidth, rectHeight;              // image rect in pixels
        float testPattern;                        // 1 = draw a grid instead of the game image
        float hudParam;                           // 0 = no readout, 1 blue, 2 red, 3 centre x, 4 centre y
        float hudValue;
        float hudDim;                             // 1 = correction off, readout greyed
    };

    D3D12Copier() = default;
    ~D3D12Copier();
    D3D12Copier(const D3D12Copier&) = delete;
    D3D12Copier& operator=(const D3D12Copier&) = delete;

    bool Init(ID3D12Device* device, ID3D12CommandQueue* queue);
    bool Ready() const { return list_ != nullptr; }
    bool ShaderReady() const { return rootSignature_ != nullptr; }

    void BeginFrame();
    void AddCopy(ID3D12Resource* src, ID3D12Resource* dst);
    // Returns false (recording nothing) if the pass can't be used for this
    // format/sample count; the caller should fall back to AddCopy.
    bool AddCorrected(ID3D12Resource* src, ID3D12Resource* dst, DXGI_FORMAT format,
                      const D3D12_RECT& rect, const CaParams& params);
    bool EndFrame();  // closes and submits if anything was recorded
    void WaitIdle();

private:
    bool InitShader();
    ID3D12PipelineState* GetPipeline(DXGI_FORMAT format);
    void BeginRecording();
    void WaitFor(uint64_t value);

    static constexpr int kFrames = 3;
    static constexpr int kDescriptorsPerFrame = 16;
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
    int descriptorsUsed_ = 0;

    ID3D12RootSignature* rootSignature_ = nullptr;
    ID3DBlob* vs_ = nullptr;
    ID3DBlob* ps_ = nullptr;
    std::unordered_map<int, ID3D12PipelineState*> pipelines_;  // by DXGI_FORMAT
    ID3D12DescriptorHeap* srvHeap_ = nullptr;  // shader visible
    ID3D12DescriptorHeap* rtvHeap_ = nullptr;
    UINT srvStride_ = 0;
    UINT rtvStride_ = 0;
};
