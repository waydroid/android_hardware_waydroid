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
 * Note: uses a single global dispatch table. This is safe for Waydroid
 * since it only ever creates a single Vulkan instance per container.
 */

#include <string.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

typedef struct {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties;
    PFN_vkGetPhysicalDeviceFormatProperties2 GetPhysicalDeviceFormatProperties2;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
} InstanceDispatch;

static InstanceDispatch g_dispatch;

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

static VKAPI_ATTR void VKAPI_CALL
compat_GetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice,
                                         VkFormat format,
                                         VkFormatProperties *pFormatProperties)
{
    g_dispatch.GetPhysicalDeviceFormatProperties(physicalDevice, format,
                                                 pFormatProperties);
    if (is_etc2_eac_format(format))
        zero_format_features(pFormatProperties);
}

static VKAPI_ATTR void VKAPI_CALL
compat_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice,
                                          VkFormat format,
                                          VkFormatProperties2 *pFormatProperties)
{
    g_dispatch.GetPhysicalDeviceFormatProperties2(physicalDevice, format,
                                                  pFormatProperties);
    if (is_etc2_eac_format(format)) {
        zero_format_features(&pFormatProperties->formatProperties);
        zero_format_features3(pFormatProperties);
    }
}

/* Instance creation: set up dispatch chain */
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

    g_dispatch.GetInstanceProcAddr = next_gipa;
    g_dispatch.DestroyInstance =
            (PFN_vkDestroyInstance)next_gipa(*pInstance, "vkDestroyInstance");
    g_dispatch.EnumeratePhysicalDevices =
            (PFN_vkEnumeratePhysicalDevices)next_gipa(
                    *pInstance, "vkEnumeratePhysicalDevices");
    g_dispatch.GetPhysicalDeviceFormatProperties =
            (PFN_vkGetPhysicalDeviceFormatProperties)next_gipa(
                    *pInstance, "vkGetPhysicalDeviceFormatProperties");
    g_dispatch.GetPhysicalDeviceFormatProperties2 =
            (PFN_vkGetPhysicalDeviceFormatProperties2)next_gipa(
                    *pInstance, "vkGetPhysicalDeviceFormatProperties2");
    g_dispatch.EnumerateDeviceExtensionProperties =
            (PFN_vkEnumerateDeviceExtensionProperties)next_gipa(
                    *pInstance, "vkEnumerateDeviceExtensionProperties");

    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
compat_DestroyInstance(VkInstance instance,
                      const VkAllocationCallbacks *pAllocator)
{
    if (g_dispatch.DestroyInstance)
        g_dispatch.DestroyInstance(instance, pAllocator);
    memset(&g_dispatch, 0, sizeof(g_dispatch));
}

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

    if (g_dispatch.GetInstanceProcAddr)
        return g_dispatch.GetInstanceProcAddr(instance, pName);
    return NULL;
}

/* Layer negotiation (loader interface version 2) */
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
