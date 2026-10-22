#include "d3d12_copier.h"

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

}  // namespace

D3D12Copier::~D3D12Copier() {
    WaitIdle();
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
    return true;
}

void D3D12Copier::WaitFor(uint64_t value) {
    if (!fence_ || fence_->GetCompletedValue() >= value) return;
    fence_->SetEventOnCompletion(value, event_);
    WaitForSingleObject(event_, INFINITE);
}

void D3D12Copier::WaitIdle() { WaitFor(fenceValue_); }

void D3D12Copier::BeginFrame() {
    recording_ = false;
    copies_ = 0;
}

void D3D12Copier::AddCopy(ID3D12Resource* src, ID3D12Resource* dst) {
    if (!recording_) {
        slot_ = (slot_ + 1) % kFrames;
        // Don't reset an allocator the GPU may still be executing from.
        WaitFor(allocatorFence_[slot_]);
        allocators_[slot_]->Reset();
        list_->Reset(allocators_[slot_], nullptr);
        recording_ = true;
    }
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
    ++copies_;
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
