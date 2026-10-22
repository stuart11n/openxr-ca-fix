#include "d3d12_copier.h"

#include <d3dcompiler.h>

#include <climits>
#include <cstring>

#include "log.h"

namespace {

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

// Full-screen triangle; the pixel shader resamples R and B around the lens
// centre in tangent space, so the correction is the same shape for both eyes
// regardless of their asymmetric frusta. With all k = 0 every sample lands on
// a texel centre and the output equals the input.
//
// For tuning it can replace the game image with a grid (run through the same
// correction, so fringes on the grid show exactly what the settings do) and
// draw a small seven-segment readout of the value being adjusted.
const char kShader[] = R"(
cbuffer Params : register(b0) {
    float4 tanBounds;  // left, right, up, down
    float2 center;
    float2 redK;
    float2 blueK;
    float2 uvOffset;
    float2 uvScale;
    float2 rectSize;
    float testPattern;
    float hudParam;
    float hudValue;
    float hudDim;
};
Texture2D<float4> src : register(t0);
SamplerState samp : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

float2 ToTan(float2 uv) { return lerp(tanBounds.xz, tanBounds.yw, uv); }
float2 ToUv(float2 t) { return (t - tanBounds.xz) / (tanBounds.yw - tanBounds.xz); }

// uv within the image rect -> where to sample this channel, also within the rect.
float2 Remap(float2 uv, float2 k) {
    float2 d = ToTan(uv) - center;
    float r2 = dot(d, d);
    float s = 1 + k.x * r2 + k.y * r2 * r2;
    return ToUv(center + d * s);
}

// White grid lines on black, 3 px wide, every 96 px, anti-aliased.
float Grid(float2 uv) {
    if (any(uv < 0) || any(uv > 1)) return 0;
    float2 px = uv * rectSize;
    float2 m = abs(frac(px / 96 + 0.5) - 0.5) * 96;
    return 1 - smoothstep(1.0, 2.5, min(m.x, m.y));
}

float4 Source(float2 uv) {
    if (testPattern > 0.5) return float4(Grid(uv).xxx, 1);
    return src.Sample(samp, uvOffset + uv * uvScale);
}

bool Box(float2 p, float2 lo, float2 hi) { return all(p >= lo) && all(p < hi); }

// Seven-segment digit; bits a..g = 0..6. p is relative to the digit's top-left.
bool Segments(uint mask, float2 p) {
    const float w = 36, h = 64, t = 9, m = h * 0.5;
    return ((mask & 1)  && Box(p, float2(0, 0),         float2(w, t))) ||          // a
           ((mask & 2)  && Box(p, float2(w - t, 0),     float2(w, m + t * 0.5))) || // b
           ((mask & 4)  && Box(p, float2(w - t, m - t * 0.5), float2(w, h))) ||     // c
           ((mask & 8)  && Box(p, float2(0, h - t),     float2(w, h))) ||           // d
           ((mask & 16) && Box(p, float2(0, m - t * 0.5), float2(t, h))) ||         // e
           ((mask & 32) && Box(p, float2(0, 0),         float2(t, m + t * 0.5))) || // f
           ((mask & 64) && Box(p, float2(0, m - t * 0.5), float2(w, m + t * 0.5))); // g
}

uint DigitMask(uint d) {
    static const uint masks[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    return masks[d % 10];
}

// Readout box just below the centre of the view: coloured border saying what
// is being adjusted, value as [-]ddd.dd. Returns alpha 0 outside the box.
float4 Hud(float2 uv) {
    if (hudParam < 0.5) return 0;
    const float2 size = float2(380, 120);
    float2 p = uv * rectSize - (float2(0.5, 0.6) * rectSize - size * 0.5);
    if (!Box(p, 0, size)) return 0;

    float3 border = hudParam < 1.5 ? float3(0.25, 0.45, 1.0)
                  : hudParam < 2.5 ? float3(1.0, 0.25, 0.2)
                  : hudParam < 3.5 ? float3(1.0, 1.0, 1.0)
                  :                  float3(1.0, 0.9, 0.1);
    if (!Box(p, 8, size - 8)) return float4(border, 1);

    float2 q = p - float2(22, 28);
    int slot = (int)floor(q.x / 48);
    float2 lp = float2(q.x - slot * 48, q.y);
    uint v = (uint)round(abs(hudValue) * 100);
    uint hundreds = (v / 10000) % 10, tens = (v / 1000) % 10;
    uint mask = 0;
    bool dot = false;
    if (slot == 0) mask = hudValue <= -0.005 ? 0x40 : 0;
    else if (slot == 1) mask = hundreds ? DigitMask(hundreds) : 0;
    else if (slot == 2) mask = (hundreds || tens) ? DigitMask(tens) : 0;
    else if (slot == 3) mask = DigitMask(v / 100);
    else if (slot == 4) dot = true;
    else if (slot == 5) mask = DigitMask(v / 10);
    else if (slot == 6) mask = DigitMask(v);
    bool lit = dot ? Box(lp, float2(13, 55), float2(23, 64)) : Segments(mask, lp);
    float3 fg = hudDim > 0.5 ? float3(0.45, 0.45, 0.45) : float3(1, 1, 1);
    return float4(lit ? fg : float3(0, 0, 0), 1);
}

float4 PSMain(VSOut i) : SV_Target {
    float4 c = Source(i.uv);
    c.r = Source(Remap(i.uv, redK)).r;
    c.b = Source(Remap(i.uv, blueK)).b;
    float4 hud = Hud(i.uv);
    return hud.a > 0 ? float4(hud.rgb, c.a) : c;
}
)";

bool Compile(const char* entry, const char* target, ID3DBlob** out) {
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "ca_fix.hlsl", nullptr, nullptr, entry,
                            target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errors);
    if (FAILED(hr)) {
        Log("shader %s compile failed 0x%08lx: %s", entry, static_cast<unsigned long>(hr),
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
    }
    SafeRelease(errors);
    return SUCCEEDED(hr);
}

}  // namespace

D3D12Copier::~D3D12Copier() {
    WaitIdle();
    for (auto& kv : pipelines_) SafeRelease(kv.second);
    SafeRelease(rootSignature_);
    SafeRelease(vs_);
    SafeRelease(ps_);
    SafeRelease(srvHeap_);
    SafeRelease(rtvHeap_);
    SafeRelease(list_);
    for (auto*& a : allocators_) SafeRelease(a);
    SafeRelease(fence_);
    SafeRelease(queue_);
    SafeRelease(device_);
    if (event_) CloseHandle(event_);
}

bool D3D12Copier::Init(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (!device || !queue) return false;
    device_ = device;
    device_->AddRef();
    queue_ = queue;
    queue_->AddRef();

    D3D12_COMMAND_QUEUE_DESC qd = queue_->GetDesc();
    Log("D3D12 queue type=%d priority=%d", static_cast<int>(qd.Type), qd.Priority);
    for (auto*& a : allocators_) {
        if (FAILED(device_->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&a)))) return false;
    }
    if (FAILED(device_->CreateCommandList(0, qd.Type, allocators_[0], nullptr, IID_PPV_ARGS(&list_)))) {
        list_ = nullptr;
        return false;
    }
    list_->Close();
    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
        SafeRelease(list_);
        return false;
    }
    event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!event_) {
        SafeRelease(list_);
        return false;
    }
    Log("D3D12 copier ready");

    // The CA pass draws, which needs a direct queue. Copies work on any queue.
    if (qd.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        Log("app queue is not a direct queue; CA pass unavailable, copying only");
    } else if (!InitShader()) {
        Log("CA pass init failed; copying only");
        SafeRelease(rootSignature_);
    } else {
        Log("CA pass ready");
    }
    return true;
}

bool D3D12Copier::InitShader() {
    if (!Compile("VSMain", "vs_5_0", &vs_) || !Compile("PSMain", "ps_5_0", &ps_)) return false;

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = sizeof(CaParams) / 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &sampler;

    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (FAILED(hr)) {
        Log("root signature serialize failed: %s",
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        SafeRelease(errors);
        return false;
    }
    hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                      IID_PPV_ARGS(&rootSignature_));
    SafeRelease(blob);
    if (FAILED(hr)) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kFrames * kDescriptorsPerFrame;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap_)))) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_)))) return false;
    srvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    rtvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
}

ID3D12PipelineState* D3D12Copier::GetPipeline(DXGI_FORMAT format) {
    auto it = pipelines_.find(format);
    if (it != pipelines_.end()) return it->second;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rootSignature_;
    pd.VS = {vs_->GetBufferPointer(), vs_->GetBufferSize()};
    pd.PS = {ps_->GetBufferPointer(), ps_->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = format;
    pd.SampleDesc.Count = 1;

    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        Log("CA pipeline for format %d failed 0x%08lx", static_cast<int>(format),
            static_cast<unsigned long>(hr));
        pso = nullptr;
    } else {
        Log("CA pipeline created for format %d", static_cast<int>(format));
    }
    pipelines_[format] = pso;  // cache failures too, so we don't retry every frame
    return pso;
}

void D3D12Copier::WaitFor(uint64_t value) {
    if (!fence_ || fence_->GetCompletedValue() >= value) return;
    fence_->SetEventOnCompletion(value, event_);
    WaitForSingleObject(event_, INFINITE);
}

void D3D12Copier::WaitIdle() { WaitFor(fenceValue_); }

void D3D12Copier::BeginFrame() {
    recording_ = false;
    descriptorsUsed_ = 0;
}

void D3D12Copier::BeginRecording() {
    if (recording_) return;
    slot_ = (slot_ + 1) % kFrames;
    // Don't reset an allocator (or reuse descriptors) the GPU may still be using.
    WaitFor(allocatorFence_[slot_]);
    allocators_[slot_]->Reset();
    list_->Reset(allocators_[slot_], nullptr);
    recording_ = true;
}

void D3D12Copier::AddCopy(ID3D12Resource* src, ID3D12Resource* dst) {
    BeginRecording();
    D3D12_RESOURCE_BARRIER before[2] = {
        Transition(src, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(dst, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    list_->ResourceBarrier(2, before);
    list_->CopyResource(dst, src);
    D3D12_RESOURCE_BARRIER after[2] = {
        Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    list_->ResourceBarrier(2, after);
}

bool D3D12Copier::AddCorrected(ID3D12Resource* src, ID3D12Resource* dst, DXGI_FORMAT format,
                               const D3D12_RECT& rect, const CaParams& params) {
    if (!ShaderReady() || descriptorsUsed_ >= kDescriptorsPerFrame) return false;
    D3D12_RESOURCE_DESC desc = src->GetDesc();
    if (desc.SampleDesc.Count != 1 || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
    ID3D12PipelineState* pso = GetPipeline(format);
    if (!pso) return false;

    BeginRecording();
    const int index = (slot_ * kDescriptorsPerFrame) + descriptorsUsed_++;

    // Views use the swapchain's declared format; the resources themselves may be typeless.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += static_cast<SIZE_T>(index) * srvStride_;
    device_->CreateShaderResourceView(src, &srv, srvCpu);
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
    srvGpu.ptr += static_cast<UINT64>(index) * srvStride_;

    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = format;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtvCpu.ptr += static_cast<SIZE_T>(index) * rtvStride_;
    device_->CreateRenderTargetView(dst, &rtv, rtvCpu);

    D3D12_RESOURCE_BARRIER before =
        Transition(src, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list_->ResourceBarrier(1, &before);

    list_->SetGraphicsRootSignature(rootSignature_);
    list_->SetPipelineState(pso);
    list_->SetDescriptorHeaps(1, &srvHeap_);
    list_->SetGraphicsRoot32BitConstants(0, sizeof(CaParams) / 4, &params, 0);
    list_->SetGraphicsRootDescriptorTable(1, srvGpu);
    list_->OMSetRenderTargets(1, &rtvCpu, FALSE, nullptr);
    D3D12_VIEWPORT vp{static_cast<float>(rect.left), static_cast<float>(rect.top),
                      static_cast<float>(rect.right - rect.left),
                      static_cast<float>(rect.bottom - rect.top), 0.0f, 1.0f};
    list_->RSSetViewports(1, &vp);
    list_->RSSetScissorRects(1, &rect);
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list_->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER after =
        Transition(src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list_->ResourceBarrier(1, &after);
    return true;
}

bool D3D12Copier::EndFrame() {
    if (!recording_) return true;
    recording_ = false;
    if (FAILED(list_->Close())) return false;
    ID3D12CommandList* lists[] = {list_};
    queue_->ExecuteCommandLists(1, lists);
    allocatorFence_[slot_] = ++fenceValue_;
    return SUCCEEDED(queue_->Signal(fence_, fenceValue_));
}
