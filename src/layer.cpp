// OpenXR API layer: per-channel radial correction of lateral chromatic aberration.
//
// The loader calls xrNegotiateLoaderApiLayerInterface, then our
// CreateApiLayerInstance in place of xrCreateInstance. We create the instance
// down the chain, look up the next layer's functions, and hand the loader our
// GetInstanceProcAddr so it routes the calls we care about through us.
//
// For each colour swapchain the app submits in a projection layer we own a
// twin "output" swapchain. At xrEndFrame we fill it from the app's last
// released image, either with a full-screen pass that resamples R and B
// radially (lateral CA correction, coefficients in config.ini) or a plain copy
// when correction is off, and submit ours instead.

#include <windows.h>

#include <d3d12.h>

#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "config.h"
#include "d3d12_copier.h"
#include "log.h"
#include "tuning.h"

namespace {

// One instance per process is the norm for games; keep the dispatch global.
PFN_xrGetInstanceProcAddr g_nextGetInstanceProcAddr = nullptr;
PFN_xrDestroyInstance g_nextDestroyInstance = nullptr;
PFN_xrCreateSession g_nextCreateSession = nullptr;
PFN_xrDestroySession g_nextDestroySession = nullptr;
PFN_xrCreateSwapchain g_nextCreateSwapchain = nullptr;
PFN_xrDestroySwapchain g_nextDestroySwapchain = nullptr;
PFN_xrEnumerateSwapchainImages g_nextEnumerateSwapchainImages = nullptr;
PFN_xrAcquireSwapchainImage g_nextAcquireSwapchainImage = nullptr;
PFN_xrWaitSwapchainImage g_nextWaitSwapchainImage = nullptr;
PFN_xrReleaseSwapchainImage g_nextReleaseSwapchainImage = nullptr;
PFN_xrEndFrame g_nextEndFrame = nullptr;
XrInstance g_instance = XR_NULL_HANDLE;
unsigned long long g_frameCount = 0;
CaConfig g_config;
TuningUi g_ui;

struct SwapchainState {
    XrSession session = XR_NULL_HANDLE;
    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    std::vector<ID3D12Resource*> appImages;  // not AddRef'd; owned by the runtime
    std::deque<uint32_t> acquired;           // indices acquired but not yet released
    int lastReleased = -1;

    // Lazily created the first time this swapchain appears in a projection layer.
    XrSwapchain output = XR_NULL_HANDLE;
    std::vector<ID3D12Resource*> outputImages;
    bool outputFailed = false;
};

struct SessionState {
    D3D12Copier copier;
};

std::mutex g_mutex;
std::unordered_map<XrSwapchain, SwapchainState> g_swapchains;
std::unordered_map<XrSession, SessionState> g_sessions;

bool IsColor(const XrSwapchainCreateInfo& ci) {
    return (ci.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
           (ci.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0;
}

const char* LayerTypeName(XrStructureType type) {
    switch (type) {
        case XR_TYPE_COMPOSITION_LAYER_PROJECTION: return "projection";
        case XR_TYPE_COMPOSITION_LAYER_QUAD: return "quad";
        default: return "other";
    }
}

// Log every frame for the first few, then once every ~10 s at 90 Hz.
bool ShouldLogFrame(unsigned long long n) { return n <= 5 || n % 900 == 0; }

// Caller holds g_mutex.
bool EnsureOutputSwapchain(XrSwapchain appSwapchain, SwapchainState& sc) {
    if (sc.output != XR_NULL_HANDLE) return true;
    if (sc.outputFailed) return false;
    sc.outputFailed = true;  // cleared on success

    XrSwapchainCreateInfo ci = sc.createInfo;
    ci.next = nullptr;
    ci.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    XrResult r = g_nextCreateSwapchain(sc.session, &ci, &sc.output);
    if (XR_FAILED(r)) {
        Log("output swapchain for %p: create failed %d", reinterpret_cast<void*>(appSwapchain), r);
        sc.output = XR_NULL_HANDLE;
        return false;
    }
    uint32_t count = 0;
    g_nextEnumerateSwapchainImages(sc.output, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    r = g_nextEnumerateSwapchainImages(sc.output, count, &count,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(r)) {
        Log("output swapchain for %p: enumerate failed %d", reinterpret_cast<void*>(appSwapchain), r);
        g_nextDestroySwapchain(sc.output);
        sc.output = XR_NULL_HANDLE;
        return false;
    }
    for (auto& img : images) sc.outputImages.push_back(img.texture);
    sc.outputFailed = false;
    Log("output swapchain %p for app %p: %ux%u fmt=%lld samples=%u array=%u images=%u",
        reinterpret_cast<void*>(sc.output), reinterpret_cast<void*>(appSwapchain), ci.width,
        ci.height, static_cast<long long>(ci.format), ci.sampleCount, ci.arraySize, count);
    return true;
}

// One app swapchain redirected this frame: the output image we acquired and
// the app image it is filled from.
struct Redirected {
    XrSwapchain app = XR_NULL_HANDLE;
    XrSwapchain output = XR_NULL_HANDLE;
    ID3D12Resource* src = nullptr;
    ID3D12Resource* dst = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool shade = false;   // use the CA pass (one draw per view)
    bool copied = false;  // plain copy recorded
};

// Acquires an output image for the app's swapchain. Returns false to pass the
// app's swapchain through unchanged. Caller holds g_mutex.
bool AcquireOutput(XrSwapchain appSwapchain, SessionState& ss, Redirected& r) {
    auto it = g_swapchains.find(appSwapchain);
    if (it == g_swapchains.end()) return false;
    SwapchainState& sc = it->second;
    if (!IsColor(sc.createInfo) || sc.lastReleased < 0 ||
        sc.lastReleased >= static_cast<int>(sc.appImages.size())) {
        return false;
    }
    if (!EnsureOutputSwapchain(appSwapchain, sc)) return false;

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(g_nextAcquireSwapchainImage(sc.output, &acquireInfo, &index))) return false;
    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(g_nextWaitSwapchainImage(sc.output, &waitInfo))) {
        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        g_nextReleaseSwapchainImage(sc.output, &releaseInfo);
        return false;
    }
    r.app = appSwapchain;
    r.output = sc.output;
    r.src = sc.appImages[sc.lastReleased];
    r.dst = sc.outputImages[index];
    r.format = static_cast<DXGI_FORMAT>(sc.createInfo.format);
    // The pass also draws the test grid and the readout, so it runs while
    // tuning even if the correction itself is off or zero.
    const bool wantPass = (g_config.enabled && !g_config.IsIdentity()) || HudVisible(g_ui, g_frameCount);
    r.shade = wantPass && ss.copier.ShaderReady() &&
              sc.createInfo.arraySize == 1 && sc.createInfo.sampleCount == 1 &&
              (sc.createInfo.usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) != 0;
    return true;
}

// Records the work that fills r.dst for one view.
void ProcessView(SessionState& ss, Redirected& r, const XrCompositionLayerProjectionView& view,
                 uint32_t eye) {
    if (r.shade) {
        D3D12_RESOURCE_DESC desc = r.src->GetDesc();
        const XrRect2Di& rect = view.subImage.imageRect;
        D3D12Copier::CaParams p{};
        p.tanLeft = tanf(view.fov.angleLeft);
        p.tanRight = tanf(view.fov.angleRight);
        p.tanUp = tanf(view.fov.angleUp);
        p.tanDown = tanf(view.fov.angleDown);
        // Config values are pixels at 45 degrees off centre (tangent 1.0).
        // Pixels per tangent unit turns them into shader coefficients: with
        // s(r) = 1 + k1*r^2 + k2*r^4, the shift at r = 1 is (k1 + k2) tangent units.
        const float pxPerTanX = static_cast<float>(rect.extent.width) / (p.tanRight - p.tanLeft);
        const float pxPerTanY = static_cast<float>(rect.extent.height) / (p.tanUp - p.tanDown);
        const uint32_t e = eye < 2 ? eye : 1;
        p.centerX = g_config.centerX[e] / pxPerTanX;
        p.centerY = g_config.centerY[e] / pxPerTanY;
        if (g_config.enabled) {
            p.redK1 = g_config.red[e] / pxPerTanX;
            p.redK2 = g_config.redOuter / pxPerTanX;
            p.blueK1 = g_config.blue[e] / pxPerTanX;
            p.blueK2 = g_config.blueOuter / pxPerTanX;
        }
        p.rectWidth = static_cast<float>(rect.extent.width);
        p.rectHeight = static_cast<float>(rect.extent.height);
        p.testPattern = g_ui.testPattern ? 1.0f : 0.0f;
        if (HudVisible(g_ui, g_frameCount) && HudInEye(g_ui, static_cast<int>(e))) {
            p.hudParam = HudParam(g_ui);
            p.hudValue = HudValue(g_config, g_ui, static_cast<int>(e));
            p.hudDim = g_config.enabled ? 0.0f : 1.0f;
        }
        p.uvOffsetX = static_cast<float>(rect.offset.x) / static_cast<float>(desc.Width);
        p.uvOffsetY = static_cast<float>(rect.offset.y) / static_cast<float>(desc.Height);
        p.uvScaleX = static_cast<float>(rect.extent.width) / static_cast<float>(desc.Width);
        p.uvScaleY = static_cast<float>(rect.extent.height) / static_cast<float>(desc.Height);
        D3D12_RECT d3dRect{rect.offset.x, rect.offset.y, rect.offset.x + rect.extent.width,
                           rect.offset.y + rect.extent.height};
        if (ss.copier.AddCorrected(r.src, r.dst, r.format, d3dRect, p)) return;
        static bool logged = false;
        if (!logged) {
            Log("CA pass unavailable for app swapchain %p; copying instead", reinterpret_cast<void*>(r.app));
            logged = true;
        }
        r.shade = false;
    }
    if (!r.copied) {
        ss.copier.AddCopy(r.src, r.dst);
        r.copied = true;
    }
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrEndFrame(XrSession session, const XrFrameEndInfo* info) {
    const unsigned long long n = ++g_frameCount;
    const bool logFrame = ShouldLogFrame(n);
    if (logFrame) {
        Log("xrEndFrame #%llu: displayTime=%lld layers=%u blend=%d", n,
            static_cast<long long>(info->displayTime), info->layerCount,
            static_cast<int>(info->environmentBlendMode));
    }

    // Copies of the projection layers and their views, with swapchains swapped
    // for ours. Everything else (quad layers, depth chained on views) is
    // forwarded untouched.
    std::vector<const XrCompositionLayerBaseHeader*> layers(info->layers,
                                                             info->layers + info->layerCount);
    std::vector<XrCompositionLayerProjection> projections;
    std::vector<std::vector<XrCompositionLayerProjectionView>> views;
    projections.reserve(info->layerCount);
    views.reserve(info->layerCount);
    std::vector<Redirected> redirected;
    redirected.reserve(4);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto sit = g_sessions.find(session);
        SessionState* ss = (sit != g_sessions.end() && sit->second.copier.Ready()) ? &sit->second : nullptr;
        if (ss) ss->copier.BeginFrame();
        // The file is read once per session (see Hook_xrCreateSession). Only with
        // live_update=1 do we keep re-reading it (about twice a second at 90 Hz)
        // and watch the keyboard.
        if (g_config.liveUpdate) {
            if (n % 45 == 0) ReloadConfigIfChanged(g_config);
            if (PollTuningKeys(g_config, g_ui, n)) SaveConfig(g_config);
        } else {
            g_ui = TuningUi();  // no grid or readout left on screen
        }

        for (uint32_t i = 0; i < info->layerCount; ++i) {
            const XrCompositionLayerBaseHeader* layer = info->layers[i];
            if (!layer) continue;
            if (layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                if (logFrame) Log("  layer %u: %s (type %d), passed through", i,
                                  LayerTypeName(layer->type), static_cast<int>(layer->type));
                continue;
            }
            auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            projections.push_back(*proj);
            views.emplace_back(proj->views, proj->views + proj->viewCount);
            auto& myViews = views.back();
            for (uint32_t v = 0; v < proj->viewCount; ++v) {
                XrSwapchain app = myViews[v].subImage.swapchain;
                XrSwapchain out = XR_NULL_HANDLE;
                if (ss) {
                    // Two views can share one swapchain (side by side or texture
                    // array); acquire its output image once.
                    Redirected* r = nullptr;
                    for (auto& e : redirected) {
                        if (e.app == app) r = &e;
                    }
                    if (!r) {
                        Redirected fresh;
                        if (AcquireOutput(app, *ss, fresh)) {
                            redirected.push_back(fresh);
                            r = &redirected.back();
                        }
                    }
                    if (r) {
                        ProcessView(*ss, *r, myViews[v], v);
                        out = r->output;
                    }
                }
                if (out) myViews[v].subImage.swapchain = out;
                if (logFrame) {
                    const XrSwapchainSubImage& sub = myViews[v].subImage;
                    Log("  layer %u view %u: app=%p -> %s=%p rect=(%d,%d %dx%d) array=%u fov=(%.3f,%.3f,%.3f,%.3f)",
                        i, v, reinterpret_cast<void*>(app), out ? "output" : "unchanged",
                        reinterpret_cast<void*>(sub.swapchain), sub.imageRect.offset.x,
                        sub.imageRect.offset.y, sub.imageRect.extent.width,
                        sub.imageRect.extent.height, sub.imageArrayIndex, myViews[v].fov.angleLeft,
                        myViews[v].fov.angleRight, myViews[v].fov.angleUp, myViews[v].fov.angleDown);
                }
            }
            projections.back().views = myViews.data();
            layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projections.back());
        }

        if (ss && !ss->copier.EndFrame()) Log("xrEndFrame #%llu: copy submit failed", n);
        for (auto& r : redirected) {
            XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            g_nextReleaseSwapchainImage(r.output, &releaseInfo);
        }
    }

    XrFrameEndInfo myInfo = *info;
    myInfo.layers = layers.data();
    XrResult result = g_nextEndFrame(session, &myInfo);
    if (XR_FAILED(result) || logFrame) Log("xrEndFrame #%llu -> %d", n, result);
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrCreateSession(XrInstance instance,
                                                    const XrSessionCreateInfo* info,
                                                    XrSession* session) {
    const XrGraphicsBindingD3D12KHR* d3d12 = nullptr;
    for (auto* s = static_cast<const XrBaseInStructure*>(info->next); s; s = s->next) {
        Log("xrCreateSession: next-chain struct type %d%s", static_cast<int>(s->type),
            s->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR ? " (D3D12 binding)" : "");
        if (s->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR)
            d3d12 = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(s);
    }
    XrResult result = g_nextCreateSession(instance, info, session);
    Log("xrCreateSession -> %d, session=%p", result,
        session ? reinterpret_cast<void*>(*session) : nullptr);
    if (XR_SUCCEEDED(result)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        ForceReloadConfig(g_config);
    }
    if (XR_SUCCEEDED(result) && d3d12) {
        std::lock_guard<std::mutex> lock(g_mutex);
        SessionState& ss = g_sessions[*session];
        if (!ss.copier.Init(d3d12->device, d3d12->queue)) {
            Log("D3D12 copier init failed; frames will pass through unchanged");
        }
    } else if (XR_SUCCEEDED(result)) {
        Log("session is not D3D12; frames will pass through unchanged");
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrDestroySession(XrSession session) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto sit = g_sessions.find(session);
        if (sit != g_sessions.end()) sit->second.copier.WaitIdle();
        for (auto it = g_swapchains.begin(); it != g_swapchains.end();) {
            if (it->second.session == session) {
                if (it->second.output) g_nextDestroySwapchain(it->second.output);
                it = g_swapchains.erase(it);
            } else {
                ++it;
            }
        }
        g_sessions.erase(session);
    }
    Log("xrDestroySession %p", reinterpret_cast<void*>(session));
    return g_nextDestroySession(session);
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrCreateSwapchain(XrSession session,
                                                      const XrSwapchainCreateInfo* info,
                                                      XrSwapchain* swapchain) {
    XrSwapchainCreateInfo ci = *info;
    // We read the app's colour images: copied when correction is off, sampled by the CA pass.
    if (IsColor(ci)) ci.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    XrResult result = g_nextCreateSwapchain(session, &ci, swapchain);
    if (XR_FAILED(result) && ci.usageFlags != info->usageFlags) {
        Log("xrCreateSwapchain with extra usage flags failed (%d), retrying as requested", result);
        ci.usageFlags = info->usageFlags;
        result = g_nextCreateSwapchain(session, &ci, swapchain);
    }
    Log("xrCreateSwapchain %ux%u fmt=%lld samples=%u array=%u faces=%u mips=%u usage=0x%llx -> %d, %p",
        info->width, info->height, static_cast<long long>(info->format), info->sampleCount,
        info->arraySize, info->faceCount, info->mipCount,
        static_cast<unsigned long long>(ci.usageFlags), result,
        XR_SUCCEEDED(result) ? reinterpret_cast<void*>(*swapchain) : nullptr);
    if (XR_SUCCEEDED(result)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        SwapchainState& sc = g_swapchains[*swapchain];
        sc.session = session;
        sc.createInfo = ci;
        sc.createInfo.next = nullptr;
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrDestroySwapchain(XrSwapchain swapchain) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_swapchains.find(swapchain);
        if (it != g_swapchains.end()) {
            if (it->second.output) {
                auto sit = g_sessions.find(it->second.session);
                if (sit != g_sessions.end()) sit->second.copier.WaitIdle();
                g_nextDestroySwapchain(it->second.output);
            }
            g_swapchains.erase(it);
        }
    }
    return g_nextDestroySwapchain(swapchain);
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrEnumerateSwapchainImages(XrSwapchain swapchain,
                                                               uint32_t capacity, uint32_t* count,
                                                               XrSwapchainImageBaseHeader* images) {
    XrResult result = g_nextEnumerateSwapchainImages(swapchain, capacity, count, images);
    if (XR_SUCCEEDED(result) && capacity > 0 && images &&
        images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_swapchains.find(swapchain);
        if (it != g_swapchains.end()) {
            auto* d3d = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
            it->second.appImages.assign(*count, nullptr);
            for (uint32_t i = 0; i < *count; ++i) it->second.appImages[i] = d3d[i].texture;
        }
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrAcquireSwapchainImage(XrSwapchain swapchain,
                                                            const XrSwapchainImageAcquireInfo* info,
                                                            uint32_t* index) {
    XrResult result = g_nextAcquireSwapchainImage(swapchain, info, index);
    if (XR_SUCCEEDED(result)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_swapchains.find(swapchain);
        if (it != g_swapchains.end()) it->second.acquired.push_back(*index);
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrReleaseSwapchainImage(XrSwapchain swapchain,
                                                            const XrSwapchainImageReleaseInfo* info) {
    XrResult result = g_nextReleaseSwapchainImage(swapchain, info);
    if (XR_SUCCEEDED(result)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_swapchains.find(swapchain);
        // Images are released in acquire order.
        if (it != g_swapchains.end() && !it->second.acquired.empty()) {
            it->second.lastReleased = static_cast<int>(it->second.acquired.front());
            it->second.acquired.pop_front();
        }
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrDestroyInstance(XrInstance instance) {
    Log("xrDestroyInstance after %llu frames", g_frameCount);
    XrResult result = g_nextDestroyInstance(instance);
    if (instance == g_instance) g_instance = XR_NULL_HANDLE;
    return result;
}

struct Hook {
    const char* name;
    PFN_xrVoidFunction fn;
};

#define HOOK(name) {#name, reinterpret_cast<PFN_xrVoidFunction>(Hook_##name)}
const Hook kHooks[] = {
    HOOK(xrDestroyInstance),          HOOK(xrCreateSession),
    HOOK(xrDestroySession),           HOOK(xrCreateSwapchain),
    HOOK(xrDestroySwapchain),         HOOK(xrEnumerateSwapchainImages),
    HOOK(xrAcquireSwapchainImage),    HOOK(xrReleaseSwapchainImage),
    HOOK(xrEndFrame),
};
#undef HOOK

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrGetInstanceProcAddr(XrInstance instance, const char* name,
                                                           PFN_xrVoidFunction* function) {
    if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;
    if (strcmp(name, "xrGetInstanceProcAddr") == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(Layer_xrGetInstanceProcAddr);
        return XR_SUCCESS;
    }
    if (instance != XR_NULL_HANDLE && g_nextGetInstanceProcAddr) {
        for (const Hook& h : kHooks) {
            if (strcmp(name, h.name) == 0) {
                *function = h.fn;
                return XR_SUCCESS;
            }
        }
    }
    if (!g_nextGetInstanceProcAddr) return XR_ERROR_HANDLE_INVALID;
    return g_nextGetInstanceProcAddr(instance, name, function);
}

template <typename T>
bool GetNext(PFN_xrGetInstanceProcAddr gipa, XrInstance instance, const char* name, T& out) {
    XrResult r = gipa(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&out));
    if (XR_FAILED(r) || !out) {
        Log("next xrGetInstanceProcAddr(%s) failed: %d", name, r);
        return false;
    }
    return true;
}

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                              const XrApiLayerCreateInfo* layerInfo,
                                                              XrInstance* instance) {
    Log("xrCreateApiLayerInstance: app=\"%s\" engine=\"%s\" extensions=%u",
        info->applicationInfo.applicationName, info->applicationInfo.engineName,
        info->enabledExtensionCount);
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        Log("  ext: %s", info->enabledExtensionNames[i]);
    }

    if (!layerInfo || !layerInfo->nextInfo ||
        strcmp(layerInfo->nextInfo->layerName, LAYER_NAME) != 0) {
        Log("xrCreateApiLayerInstance: bad layer chain");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    // Advance the chain past ourselves before calling down.
    XrApiLayerCreateInfo nextLayerInfo = *layerInfo;
    nextLayerInfo.nextInfo = layerInfo->nextInfo->next;
    PFN_xrGetInstanceProcAddr nextGipa = layerInfo->nextInfo->nextGetInstanceProcAddr;

    XrResult result = layerInfo->nextInfo->nextCreateApiLayerInstance(info, &nextLayerInfo, instance);
    if (XR_FAILED(result)) {
        Log("xrCreateApiLayerInstance: next layer failed: %d", result);
        return result;
    }

    g_instance = *instance;
    bool ok = GetNext(nextGipa, *instance, "xrDestroyInstance", g_nextDestroyInstance) &&
              GetNext(nextGipa, *instance, "xrCreateSession", g_nextCreateSession) &&
              GetNext(nextGipa, *instance, "xrDestroySession", g_nextDestroySession) &&
              GetNext(nextGipa, *instance, "xrCreateSwapchain", g_nextCreateSwapchain) &&
              GetNext(nextGipa, *instance, "xrDestroySwapchain", g_nextDestroySwapchain) &&
              GetNext(nextGipa, *instance, "xrEnumerateSwapchainImages", g_nextEnumerateSwapchainImages) &&
              GetNext(nextGipa, *instance, "xrAcquireSwapchainImage", g_nextAcquireSwapchainImage) &&
              GetNext(nextGipa, *instance, "xrWaitSwapchainImage", g_nextWaitSwapchainImage) &&
              GetNext(nextGipa, *instance, "xrReleaseSwapchainImage", g_nextReleaseSwapchainImage) &&
              GetNext(nextGipa, *instance, "xrEndFrame", g_nextEndFrame);
    if (!ok) {
        // Without the full dispatch table we can't intercept safely; stay out of the way.
        Log("missing next functions; layer disabled for this instance");
        g_nextGetInstanceProcAddr = nullptr;
        return XR_SUCCESS;
    }
    // Set last: Layer_xrGetInstanceProcAddr only hooks once this is non-null.
    g_nextGetInstanceProcAddr = nextGipa;

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    PFN_xrGetInstanceProperties getProps = nullptr;
    if (XR_SUCCEEDED(nextGipa(*instance, "xrGetInstanceProperties",
                              reinterpret_cast<PFN_xrVoidFunction*>(&getProps))) &&
        XR_SUCCEEDED(getProps(*instance, &props))) {
        Log("runtime: %s %u.%u.%u", props.runtimeName,
            XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion),
            XR_VERSION_PATCH(props.runtimeVersion));
    }
    Log("instance %p created", reinterpret_cast<void*>(*instance));
    return XR_SUCCESS;
}

}  // namespace

extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loaderInfo, const char* layerName,
    XrNegotiateApiLayerRequest* request) {
    Log("%s loaded, negotiating", LAYER_NAME);
    if (!loaderInfo || !request || !layerName || strcmp(layerName, LAYER_NAME) != 0 ||
        loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo) ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        request->structSize != sizeof(XrNegotiateApiLayerRequest) ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION) {
        Log("negotiation failed");
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = XR_API_VERSION_1_0;
    request->getInstanceProcAddr = Layer_xrGetInstanceProcAddr;
    request->createApiLayerInstance = Layer_xrCreateApiLayerInstance;
    return XR_SUCCESS;
}
