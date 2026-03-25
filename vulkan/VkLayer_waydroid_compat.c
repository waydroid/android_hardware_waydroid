/*
 * Copyright (C) 2026 The Waydroid Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Vulkan implicit layer that masks ETC2/EAC format support to prevent
 * crashes on Intel GPUs when ARM64 apps use AHardwareBuffer with
 * compressed textures.
 *
 * Mesa ANV does not handle AHardwareBuffer with ETC2 formats through
 * gralloc, causing a SIGABRT in AHardwareBuffer_getNativeHandle when
 * gralloc encounters unsupported format 0x38 (ETC2_RGB8).
 *
 * This layer intercepts vkGetPhysicalDeviceFormatProperties{,2} and
 * reports zero feature flags for all ETC2/EAC formats, causing apps
 * to fall back to uncompressed textures.
 *
 * Dispatch tables are stored per-instance to correctly handle multiple
 * VkInstance objects (e.g. from apps and native services).
 */

#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "VkLayer_waydroid_compat"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGE(...) ((void)0)
#endif

/* ---- Per-instance dispatch ---- */

typedef struct {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties;
    PFN_vkGetPhysicalDeviceFormatProperties2 GetPhysicalDeviceFormatProperties2;
} InstanceDispatch;

typedef struct InstanceDispatchEntry {
    VkInstance instance;
    InstanceDispatch dispatch;
    struct InstanceDispatchEntry *next;
} InstanceDispatchEntry;

static InstanceDispatchEntry *g_instances = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Caller must hold g_lock */
static InstanceDispatch *find_instance_dispatch_locked(VkInstance instance)
{
    InstanceDispatchEntry *entry = g_instances;
    while (entry) {
        if (entry->instance == instance)
            return &entry->dispatch;
        entry = entry->next;
    }
    return NULL;
}

/*
 * The Vulkan loader sets the first pointer-sized field of any dispatchable
 * handle (VkInstance, VkPhysicalDevice, VkDevice, etc.) to point to the
 * loader's dispatch table. Physical devices share this key with their
 * parent instance (the loader guarantees this). We use it to map a
 * VkPhysicalDevice back to the instance dispatch we stored at creation.
 */
/* Caller must hold g_lock */
static InstanceDispatch *find_phys_dev_dispatch_locked(VkPhysicalDevice physicalDevice)
{
    void *key = *(void **)physicalDevice;
    InstanceDispatchEntry *entry = g_instances;
    while (entry) {
        if (*(void **)entry->instance == key)
            return &entry->dispatch;
        entry = entry->next;
    }
    return NULL;
}

/* ---- ETC2/EAC format masking ---- */

static int is_etc2_eac_format(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
    case VK_FORMAT_EAC_R11_UNORM_BLOCK:
    case VK_FORMAT_EAC_R11_SNORM_BLOCK:
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
    case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
        return 1;
    default:
        return 0;
    }
}

static void zero_format_features(VkFormatProperties *props)
{
    props->linearTilingFeatures = 0;
    props->optimalTilingFeatures = 0;
    props->bufferFeatures = 0;
}

/* Walk the pNext chain and zero out VkFormatProperties3 if present */
static void zero_format_features3(VkFormatProperties2 *pFormatProperties)
{
    VkBaseOutStructure *ext = (VkBaseOutStructure *)pFormatProperties->pNext;
    while (ext) {
        if (ext->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
            VkFormatProperties3 *props3 = (VkFormatProperties3 *)ext;
            props3->linearTilingFeatures = 0;
            props3->optimalTilingFeatures = 0;
            props3->bufferFeatures = 0;
            break;
        }
        ext = ext->pNext;
    }
}

/* ---- Intercepted entry points ---- */

static VKAPI_ATTR void VKAPI_CALL
compat_GetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice,
                                         VkFormat format,
                                         VkFormatProperties *pFormatProperties)
{
    PFN_vkGetPhysicalDeviceFormatProperties fn = NULL;

    pthread_mutex_lock(&g_lock);
    InstanceDispatch *dispatch = find_phys_dev_dispatch_locked(physicalDevice);
    if (dispatch)
        fn = dispatch->GetPhysicalDeviceFormatProperties;
    pthread_mutex_unlock(&g_lock);

    if (!fn) {
        LOGE("dispatch lookup failed for vkGetPhysicalDeviceFormatProperties");
        return;
    }
    fn(physicalDevice, format, pFormatProperties);
    if (is_etc2_eac_format(format))
        zero_format_features(pFormatProperties);
}

static VKAPI_ATTR void VKAPI_CALL
compat_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice,
                                          VkFormat format,
                                          VkFormatProperties2 *pFormatProperties)
{
    PFN_vkGetPhysicalDeviceFormatProperties2 fn = NULL;

    pthread_mutex_lock(&g_lock);
    InstanceDispatch *dispatch = find_phys_dev_dispatch_locked(physicalDevice);
    if (dispatch)
        fn = dispatch->GetPhysicalDeviceFormatProperties2;
    pthread_mutex_unlock(&g_lock);

    if (!fn) {
        LOGE("dispatch lookup failed for vkGetPhysicalDeviceFormatProperties2");
        return;
    }
    fn(physicalDevice, format, pFormatProperties);
    if (is_etc2_eac_format(format)) {
        zero_format_features(&pFormatProperties->formatProperties);
        zero_format_features3(pFormatProperties);
    }
}

/* ---- Instance lifecycle ---- */

static VKAPI_ATTR VkResult VKAPI_CALL
compat_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkInstance *pInstance)
{
    VkLayerInstanceCreateInfo *layer_info =
            (VkLayerInstanceCreateInfo *)pCreateInfo->pNext;

    while (layer_info &&
           (layer_info->sType !=
                    VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
            layer_info->function != VK_LAYER_LINK_INFO)) {
        layer_info = (VkLayerInstanceCreateInfo *)layer_info->pNext;
    }

    if (!layer_info)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa =
            layer_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;

    /* Advance the chain for the next layer */
    layer_info->u.pLayerInfo = layer_info->u.pLayerInfo->pNext;

    PFN_vkCreateInstance create_instance =
            (PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create_instance)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkResult result = create_instance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS)
        return result;

    PFN_vkDestroyInstance destroy_fn =
            (PFN_vkDestroyInstance)next_gipa(*pInstance, "vkDestroyInstance");

    /* Allocate and populate a per-instance dispatch entry */
    InstanceDispatchEntry *entry =
            (InstanceDispatchEntry *)malloc(sizeof(InstanceDispatchEntry));
    if (!entry) {
        LOGE("malloc failed for InstanceDispatchEntry");
        if (destroy_fn)
            destroy_fn(*pInstance, pAllocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    entry->instance = *pInstance;
    entry->dispatch.GetInstanceProcAddr = next_gipa;
    entry->dispatch.DestroyInstance = destroy_fn;
    entry->dispatch.GetPhysicalDeviceFormatProperties =
            (PFN_vkGetPhysicalDeviceFormatProperties)next_gipa(
                    *pInstance, "vkGetPhysicalDeviceFormatProperties");
    entry->dispatch.GetPhysicalDeviceFormatProperties2 =
            (PFN_vkGetPhysicalDeviceFormatProperties2)next_gipa(
                    *pInstance, "vkGetPhysicalDeviceFormatProperties2");

    /* Prepend to linked list */
    pthread_mutex_lock(&g_lock);
    entry->next = g_instances;
    g_instances = entry;
    pthread_mutex_unlock(&g_lock);

    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
compat_DestroyInstance(VkInstance instance,
                      const VkAllocationCallbacks *pAllocator)
{
    PFN_vkDestroyInstance destroy_fn = NULL;

    pthread_mutex_lock(&g_lock);
    InstanceDispatchEntry **prev = &g_instances;
    InstanceDispatchEntry *entry = g_instances;

    while (entry) {
        if (entry->instance == instance) {
            *prev = entry->next;
            destroy_fn = entry->dispatch.DestroyInstance;
            free(entry);
            break;
        }
        prev = &entry->next;
        entry = entry->next;
    }
    pthread_mutex_unlock(&g_lock);

    if (destroy_fn)
        destroy_fn(instance, pAllocator);
}

/* ---- GetInstanceProcAddr ---- */

__attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
compat_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
    if (strcmp(pName, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)compat_CreateInstance;
    if (strcmp(pName, "vkDestroyInstance") == 0)
        return (PFN_vkVoidFunction)compat_DestroyInstance;
    if (strcmp(pName, "vkGetPhysicalDeviceFormatProperties") == 0)
        return (PFN_vkVoidFunction)compat_GetPhysicalDeviceFormatProperties;
    if (strcmp(pName, "vkGetPhysicalDeviceFormatProperties2") == 0)
        return (PFN_vkVoidFunction)compat_GetPhysicalDeviceFormatProperties2;
    if (strcmp(pName, "vkGetPhysicalDeviceFormatProperties2KHR") == 0)
        return (PFN_vkVoidFunction)compat_GetPhysicalDeviceFormatProperties2;

    PFN_vkGetInstanceProcAddr gipa = NULL;

    pthread_mutex_lock(&g_lock);
    InstanceDispatch *dispatch = find_instance_dispatch_locked(instance);
    if (dispatch)
        gipa = dispatch->GetInstanceProcAddr;
    pthread_mutex_unlock(&g_lock);

    if (gipa)
        return gipa(instance, pName);
    return NULL;
}

/* ---- Layer negotiation (loader interface version 2) ---- */

__attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(
        VkNegotiateLayerInterface *pVersionStruct)
{
    if (!pVersionStruct ||
        pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (pVersionStruct->loaderLayerInterfaceVersion >=
        CURRENT_LOADER_LAYER_INTERFACE_VERSION) {
        pVersionStruct->loaderLayerInterfaceVersion =
                CURRENT_LOADER_LAYER_INTERFACE_VERSION;
    }

    pVersionStruct->pfnGetInstanceProcAddr = compat_GetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = NULL;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = NULL;

    return VK_SUCCESS;
}

/* ---- Layer enumeration ---- */

static const VkLayerProperties layer_props = {
        .layerName = "VK_LAYER_WAYDROID_compat",
        .specVersion = VK_MAKE_VERSION(1, 3, 0),
        .implementationVersion = 1,
        .description = "Waydroid ETC2/EAC format compatibility layer",
};

__attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                   VkLayerProperties *pProperties)
{
    if (!pProperties) {
        *pPropertyCount = 1;
        return VK_SUCCESS;
    }
    if (*pPropertyCount < 1)
        return VK_INCOMPLETE;

    *pPropertyCount = 1;
    memcpy(pProperties, &layer_props, sizeof(layer_props));
    return VK_SUCCESS;
}

__attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties(const char *pLayerName,
                                       uint32_t *pPropertyCount,
                                       VkExtensionProperties *pProperties)
{
    (void)pProperties;
    if (pLayerName && strcmp(pLayerName, layer_props.layerName) == 0) {
        *pPropertyCount = 0;
        return VK_SUCCESS;
    }
    return VK_ERROR_LAYER_NOT_PRESENT;
}
