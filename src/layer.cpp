// Step 1: pass-through OpenXR API layer that logs xrEndFrame.
//
// The loader calls xrNegotiateLoaderApiLayerInterface, then our
// CreateApiLayerInstance in place of xrCreateInstance. We create the instance
// down the chain, look up the next layer's functions, and hand the loader our
// GetInstanceProcAddr so it routes the calls we care about through us.

#include <windows.h>

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <cstring>

#include "log.h"

namespace {

// One instance per process is the norm for games; keep the dispatch global.
PFN_xrGetInstanceProcAddr g_nextGetInstanceProcAddr = nullptr;
PFN_xrDestroyInstance g_nextDestroyInstance = nullptr;
PFN_xrCreateSession g_nextCreateSession = nullptr;
PFN_xrEndFrame g_nextEndFrame = nullptr;
XrInstance g_instance = XR_NULL_HANDLE;
unsigned long long g_frameCount = 0;

const char* LayerTypeName(XrStructureType type) {
    switch (type) {
        case XR_TYPE_COMPOSITION_LAYER_PROJECTION: return "projection";
        case XR_TYPE_COMPOSITION_LAYER_QUAD: return "quad";
        default: return "other";
    }
}

// Log every frame for the first few, then once every ~10 s at 90 Hz.
bool ShouldLogFrame(unsigned long long n) { return n <= 5 || n % 900 == 0; }

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrEndFrame(XrSession session, const XrFrameEndInfo* info) {
    const unsigned long long n = ++g_frameCount;
    if (ShouldLogFrame(n)) {
        Log("xrEndFrame #%llu: displayTime=%lld layers=%u blend=%d", n,
            static_cast<long long>(info->displayTime), info->layerCount,
            static_cast<int>(info->environmentBlendMode));
        for (uint32_t i = 0; i < info->layerCount; ++i) {
            const XrCompositionLayerBaseHeader* layer = info->layers[i];
            if (!layer) continue;
            if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
                Log("  layer %u: projection, %u views, flags=0x%llx", i, proj->viewCount,
                    static_cast<unsigned long long>(proj->layerFlags));
                for (uint32_t v = 0; v < proj->viewCount; ++v) {
                    const XrSwapchainSubImage& sub = proj->views[v].subImage;
                    Log("    view %u: swapchain=%p rect=(%d,%d %dx%d) array=%u", v,
                        reinterpret_cast<void*>(sub.swapchain), sub.imageRect.offset.x,
                        sub.imageRect.offset.y, sub.imageRect.extent.width,
                        sub.imageRect.extent.height, sub.imageArrayIndex);
                }
            } else {
                Log("  layer %u: %s (type %d)", i, LayerTypeName(layer->type),
                    static_cast<int>(layer->type));
            }
        }
    }
    XrResult result = g_nextEndFrame(session, info);
    if (XR_FAILED(result) || ShouldLogFrame(n)) Log("xrEndFrame #%llu -> %d", n, result);
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrCreateSession(XrInstance instance,
                                                    const XrSessionCreateInfo* info,
                                                    XrSession* session) {
    // Step 2 will pull the ID3D12Device and queue out of this chain; for now just
    // show which graphics binding the game uses.
    for (auto* s = static_cast<const XrBaseInStructure*>(info->next); s; s = s->next) {
        Log("xrCreateSession: next-chain struct type %d%s", static_cast<int>(s->type),
            s->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR ? " (D3D12 binding)" : "");
    }
    XrResult result = g_nextCreateSession(instance, info, session);
    Log("xrCreateSession -> %d, session=%p", result,
        session ? reinterpret_cast<void*>(*session) : nullptr);
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrDestroyInstance(XrInstance instance) {
    Log("xrDestroyInstance after %llu frames", g_frameCount);
    XrResult result = g_nextDestroyInstance(instance);
    if (instance == g_instance) g_instance = XR_NULL_HANDLE;
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrGetInstanceProcAddr(XrInstance instance, const char* name,
                                                           PFN_xrVoidFunction* function) {
    if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;
    if (strcmp(name, "xrGetInstanceProcAddr") == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(Layer_xrGetInstanceProcAddr);
        return XR_SUCCESS;
    }
    if (instance != XR_NULL_HANDLE && g_nextGetInstanceProcAddr) {
        if (strcmp(name, "xrEndFrame") == 0) {
            *function = reinterpret_cast<PFN_xrVoidFunction>(Hook_xrEndFrame);
            return XR_SUCCESS;
        }
        if (strcmp(name, "xrCreateSession") == 0) {
            *function = reinterpret_cast<PFN_xrVoidFunction>(Hook_xrCreateSession);
            return XR_SUCCESS;
        }
        if (strcmp(name, "xrDestroyInstance") == 0) {
            *function = reinterpret_cast<PFN_xrVoidFunction>(Hook_xrDestroyInstance);
            return XR_SUCCESS;
        }
    }
    if (!g_nextGetInstanceProcAddr) return XR_ERROR_HANDLE_INVALID;
    return g_nextGetInstanceProcAddr(instance, name, function);
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

    g_nextGetInstanceProcAddr = nextGipa;
    g_instance = *instance;
    nextGipa(*instance, "xrDestroyInstance",
             reinterpret_cast<PFN_xrVoidFunction*>(&g_nextDestroyInstance));
    nextGipa(*instance, "xrCreateSession",
             reinterpret_cast<PFN_xrVoidFunction*>(&g_nextCreateSession));
    nextGipa(*instance, "xrEndFrame", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextEndFrame));

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
