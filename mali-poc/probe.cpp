// Copyright 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Mali host-renderer capability probe.
//
// This binary is meant to run on an Android device (arm64, bionic) as a normal
// app or as root from /data/local/tmp. It answers three questions that decide
// whether gfxstream can be fixed for Mali with a host-side patch:
//
//   A. Which (format, tiling, createFlags, usage) combinations does the Mali
//      Vulkan driver accept when the image is backed by an
//      AHardwareBuffer (the query gfxstream performs in
//      VkEmulation::populateImageFormatExternalMemorySupportInfo())?
//   B. When gfxstream's own choice (B8G8R8A8_UNORM + OPTIMAL + MUTABLE_FORMAT |
//      EXTENDED_USAGE + full usage flags) fails, which relaxed combination
//      works end to end (AHB allocate -> VkImage -> import -> bind -> view)?
//   C. Which GLES call in gfxstream's TextureDraw::TextureDraw() constructor
//      raises GL_INVALID_ENUM (0x500) on Mali?
//
// The output is line oriented so it can be pasted into a bug report or diffed
// between devices.

#define VK_USE_PLATFORM_ANDROID_KHR 1

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#define PROBE_TAG "MaliProbe"

#define PLOG(fmt, ...)                              \
    do {                                            \
        printf(fmt "\n", ##__VA_ARGS__);            \
        fflush(stdout);                             \
        __android_log_print(ANDROID_LOG_INFO, PROBE_TAG, fmt, ##__VA_ARGS__); \
    } while (0)

#define PERR(fmt, ...)                              \
    do {                                            \
        printf(fmt "\n", ##__VA_ARGS__);            \
        fflush(stdout);                             \
        __android_log_print(ANDROID_LOG_ERROR, PROBE_TAG, fmt, ##__VA_ARGS__); \
    } while (0)

// HAL_PIXEL_FORMAT_BGRA_8888. gfxstream uses this raw value for
// VK_FORMAT_B8G8R8A8_UNORM color buffers (host/vulkan/vk_common_operations.cpp
// ComputeAhbShapeKey). AOSP exposes it as AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM
// but the NDK header does not always define it.
#ifndef AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM
#define AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM 5u
#endif

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const char* vkResStr(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "OK";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "NS";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "E_OOM_HOST";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "E_OOM_DEV";
        case VK_ERROR_INITIALIZATION_FAILED: return "E_INIT";
        case VK_ERROR_DEVICE_LOST: return "E_LOST";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "E_EXT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "E_FEAT";
        default: {
            static char buf[32];
            snprintf(buf, sizeof(buf), "E_%d", (int)r);
            return buf;
        }
    }
}

std::string usageStr(VkImageUsageFlags usage) {
    std::string s;
    auto add = [&s](const char* name) {
        if (!s.empty()) s += "|";
        s += name;
    };
    if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) add("CA");
    if (usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) add("IA");
    if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) add("DS");
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) add("SMP");
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT) add("STO");
    if (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) add("TSRC");
    if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) add("TDST");
    if (s.empty()) s = "0";
    return s;
}

std::string flagsStr(VkImageCreateFlags flags) {
    std::string s;
    if (flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) s += "MUT";
    if (flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) {
        if (!s.empty()) s += "|";
        s += "EXT";
    }
    if (s.empty()) s = "0";
    return s;
}

// Mirrors gfxstream getSupportedImageUsageFlags().
VkImageUsageFlags gfxstreamUsageFlags(const VkFormatProperties& props, VkImageTiling tiling) {
    const VkFormatFeatureFlags features = (tiling == VK_IMAGE_TILING_OPTIMAL)
                                              ? props.optimalTilingFeatures
                                              : props.linearTilingFeatures;
    VkImageUsageFlags usage = 0;
    if (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) {
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    }
    if (features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    }
    if (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (features & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (features & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) {
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (features & VK_FORMAT_FEATURE_BLIT_SRC_BIT) {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    return usage;
}

struct FormatEntry {
    VkFormat format;
    const char* name;
    uint32_t ahbFormat;  // 0 when unmapped
};

const FormatEntry kFormats[] = {
    {VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM", AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM},
    {VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM},
    {VK_FORMAT_R8G8B8A8_SRGB, "R8G8B8A8_SRGB", AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM},
    {VK_FORMAT_R8G8B8_UNORM, "R8G8B8_UNORM", 0},
    {VK_FORMAT_R5G6B5_UNORM_PACK16, "R5G6B5_UNORM", AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM},
    {VK_FORMAT_A1R5G5B5_UNORM_PACK16, "A1R5G5B5_UNORM", 0},
    {VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT", 0},
    {VK_FORMAT_R8_UNORM, "R8_UNORM", 0},
    {VK_FORMAT_R16_UNORM, "R16_UNORM", 0},
    {VK_FORMAT_B4G4R4A4_UNORM_PACK16, "B4G4R4A4_UNORM", 0},
    {VK_FORMAT_R4G4B4A4_UNORM_PACK16, "R4G4B4A4_UNORM", 0},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM", 0},
    {VK_FORMAT_A2R10G10B10_UNORM_PACK32, "A2R10G10B10_UNORM", 0},
    {VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, "ETC2_RGBA8", 0},
    {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC_4x4_UNORM", 0},
    {VK_FORMAT_D32_SFLOAT, "D32_SFLOAT", 0},
};

// ---------------------------------------------------------------------------
// Vulkan state
// ---------------------------------------------------------------------------

VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_phys = VK_NULL_HANDLE;
VkDevice g_dev = VK_NULL_HANDLE;
uint32_t g_queueFamily = ~0u;
bool g_ahbExtension = false;
PFN_vkGetAndroidHardwareBufferPropertiesANDROID g_ahbProps = nullptr;

bool initVulkan() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "mali_probe";
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instanceCi{};
    instanceCi.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceCi.pApplicationInfo = &appInfo;

    VkResult res = vkCreateInstance(&instanceCi, nullptr, &g_instance);
    if (res != VK_SUCCESS) {
        PERR("A0 vkCreateInstance failed: %s", vkResStr(res));
        return false;
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g_instance, &count, nullptr);
    if (count == 0) {
        PERR("A0 no Vulkan physical devices");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(g_instance, &count, devices.data());

    // Prefer the discrete/integrated GPU by deviceType, same way gfxstream's
    // device selection does.
    g_phys = devices[0];
    VkPhysicalDeviceProperties props{};
    for (auto pd : devices) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pd, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
            p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            g_phys = pd;
            props = p;
            break;
        }
    }
    vkGetPhysicalDeviceProperties(g_phys, &props);

    PLOG("A0 device: %s", props.deviceName);
    PLOG("A0 apiVersion: %u.%u.%u driverVersion: 0x%08x vendorId: 0x%04x deviceId: 0x%04x",
         VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
         VK_VERSION_PATCH(props.apiVersion), props.driverVersion, props.vendorID,
         props.deviceID);
    PLOG("A0 maxImageDimension2D=%u maxFramebufferWidth=%u", props.limits.maxImageDimension2D,
         props.limits.maxFramebufferWidth);

    // Device extension support.
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(g_phys, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(g_phys, nullptr, &extCount, exts.data());
    std::string ahbExt = "no";
    std::string externalMemoryExt = "no";
    for (const auto& e : exts) {
        if (strcmp(e.extensionName, "VK_ANDROID_external_memory_android_hardware_buffer") == 0) {
            g_ahbExtension = true;
            ahbExt = "yes";
        }
        if (strcmp(e.extensionName, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) == 0) {
            externalMemoryExt = "yes";
        }
    }
    PLOG("A0 extensions: %s=%s %s=%s", "VK_ANDROID_external_memory_android_hardware_buffer",
         ahbExt.c_str(), VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, externalMemoryExt.c_str());

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(g_phys, &memProps);
    PLOG("A0 memory: heapCount=%u typeCount=%u", memProps.memoryHeapCount,
         memProps.memoryTypeCount);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        PLOG("A0   type[%u] flags=0x%x heap=%u", i, memProps.memoryTypes[i].propertyFlags,
             memProps.memoryTypes[i].heapIndex);
    }

    // Create a device with a graphics queue (needed for image creation tests).
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qfCount, qfs.data());
    for (uint32_t i = 0; i < qfCount; i++) {
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            g_queueFamily = i;
            break;
        }
    }
    if (g_queueFamily == ~0u) {
        PERR("A0 no graphics queue family");
        return true;  // queries still work without a device
    }

    std::vector<const char*> enabledExts;
    if (g_ahbExtension) {
        enabledExts.push_back("VK_ANDROID_external_memory_android_hardware_buffer");
    }
    if (externalMemoryExt == "yes") {
        enabledExts.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueCi{};
    queueCi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCi.queueFamilyIndex = g_queueFamily;
    queueCi.queueCount = 1;
    queueCi.pQueuePriorities = &priority;

    VkDeviceCreateInfo deviceCi{};
    deviceCi.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCi.queueCreateInfoCount = 1;
    deviceCi.pQueueCreateInfos = &queueCi;
    deviceCi.enabledExtensionCount = (uint32_t)enabledExts.size();
    deviceCi.ppEnabledExtensionNames = enabledExts.data();

    res = vkCreateDevice(g_phys, &deviceCi, nullptr, &g_dev);
    if (res != VK_SUCCESS) {
        PERR("A0 vkCreateDevice failed: %s", vkResStr(res));
        g_dev = VK_NULL_HANDLE;
        return true;
    }
    PLOG("A0 vkCreateDevice ok (queueFamily=%u, exts=%zu)", g_queueFamily, enabledExts.size());
    g_ahbProps = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(
        g_dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
    PLOG("A0 vkGetAndroidHardwareBufferPropertiesANDROID: %s",
         g_ahbProps ? "available" : "MISSING");
    return true;
}

// Mirrors the query in VkEmulation::populateImageFormatExternalMemorySupportInfo().
VkResult queryImageFormat(VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
                          VkImageCreateFlags createFlags,
                          VkExternalMemoryFeatureFlags* outFeatures = nullptr) {
    VkPhysicalDeviceImageFormatInfo2 formatInfo2{};
    formatInfo2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    formatInfo2.format = format;
    formatInfo2.type = VK_IMAGE_TYPE_2D;
    formatInfo2.tiling = tiling;
    formatInfo2.usage = usage;
    formatInfo2.flags = createFlags;

    VkPhysicalDeviceExternalImageFormatInfo extInfo{};
    extInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    extInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

    VkExternalImageFormatProperties outExternalProps{};
    outExternalProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;

    VkImageFormatProperties2 outProps2{};
    outProps2.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    outProps2.pNext = &outExternalProps;

    if (g_ahbExtension) {
        formatInfo2.pNext = &extInfo;
    }

    const VkResult res = vkGetPhysicalDeviceImageFormatProperties2(g_phys, &formatInfo2, &outProps2);
    if (outFeatures) {
        *outFeatures = outExternalProps.externalMemoryProperties.externalMemoryFeatures;
    }
    return res;
}

void sectionA() {
    PLOG("");
    PLOG("=== A. gfxstream format support matrix (AHB external memory) ===");
    PLOG("columns: usage=full (gfxstream), noIA (full minus INPUT_ATTACHMENT),");
    PLOG("         CA (CA|SMP|TSRC|TDST), CAonly; flags=MUT|EXT adds MUTABLE_FORMAT|EXTENDED_USAGE");

    const VkImageTiling tilings[] = {VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_TILING_LINEAR};
    const char* tilingNames[] = {"OPTIMAL", "LINEAR"};

    for (const auto& f : kFormats) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(g_phys, f.format, &props);
        PLOG("");
        PLOG("%s format=%d optFeatures=0x%08x linFeatures=0x%08x", f.name, (int)f.format,
             props.optimalTilingFeatures, props.linearTilingFeatures);

        for (int t = 0; t < 2; t++) {
            const VkImageTiling tiling = tilings[t];
            const VkImageUsageFlags full = gfxstreamUsageFlags(props, tiling);
            if (full == 0) {
                PLOG("  %-7s usage=0 (format unusable for this tiling)", tilingNames[t]);
                continue;
            }
            const VkImageUsageFlags noIa = full & ~VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
            const VkImageUsageFlags ca = (VkImageUsageFlags)(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                             VK_IMAGE_USAGE_SAMPLED_BIT |
                                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                             VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            const VkImageUsageFlags caOnly = (VkImageUsageFlags)VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

            for (int flagMode = 0; flagMode < 2; flagMode++) {
                const VkImageCreateFlags flags =
                    (flagMode == 0) ? 0
                                    : (VkImageCreateFlags)(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                                                           VK_IMAGE_CREATE_EXTENDED_USAGE_BIT);
                char line[512];
                snprintf(line, sizeof(line), "  %-7s f=%-7s full(%-28s)=%-4s noIA=%-4s CA=%-4s CAonly=%-4s",
                         tilingNames[t], flagsStr(flags).c_str(), usageStr(full).c_str(),
                         vkResStr(queryImageFormat(f.format, tiling, full, flags)),
                         vkResStr(queryImageFormat(f.format, tiling, noIa, flags)),
                         vkResStr(queryImageFormat(f.format, tiling, ca, flags)),
                         vkResStr(queryImageFormat(f.format, tiling, caOnly, flags)));
                PLOG("%s", line);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Section B: end-to-end color buffer flow
// ---------------------------------------------------------------------------

struct ImageAttempt {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    AHardwareBuffer* ahb = nullptr;
    uint32_t ahbFormat = 0;
    uint64_t ahbUsage = 0;
    std::string failure;
};

uint64_t ahbUsageForVkUsage(VkImageUsageFlags usage) {
    // Mirrors gfxstream ComputeAhbShapeKey().
    uint64_t ahbUsage = 0;
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        ahbUsage |= AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    }
    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) {
        ahbUsage |= AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER;
    }
    if (ahbUsage == 0) {
        ahbUsage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    }
    return ahbUsage;
}

ImageAttempt attemptColorBuffer(const FormatEntry& f, uint32_t width, uint32_t height,
                                VkImageTiling tiling, VkImageUsageFlags usage,
                                VkImageCreateFlags createFlags, const char* label) {
    ImageAttempt attempt;
    PLOG("");
    PLOG("--- B: %s", label);
    PLOG("    format=%s tiling=%d usage=%s flags=%s", f.name, (int)tiling, usageStr(usage).c_str(),
         flagsStr(createFlags).c_str());

    const VkResult queryRes = queryImageFormat(f.format, tiling, usage, createFlags);
    PLOG("    query: %s", vkResStr(queryRes));
    if (queryRes != VK_SUCCESS) {
        attempt.failure = "query";
        return attempt;
    }

    if (f.ahbFormat == 0) {
        PLOG("    skip: no AHB format mapping");
        attempt.failure = "no_ahb_mapping";
        return attempt;
    }
    if (!g_dev || !g_ahbProps) {
        PLOG("    skip: no device / AHB props function");
        attempt.failure = "no_device";
        return attempt;
    }

    attempt.ahbFormat = f.ahbFormat;
    attempt.ahbUsage = ahbUsageForVkUsage(usage);
    AHardwareBuffer_Desc desc{};
    desc.width = width;
    desc.height = height;
    desc.layers = 1;
    desc.format = f.ahbFormat;
    desc.usage = attempt.ahbUsage;
    int ahbRes = AHardwareBuffer_allocate(&desc, &attempt.ahb);
    PLOG("    AHardwareBuffer_allocate(format=%u usage=0x%llx) -> %d", f.ahbFormat,
         (unsigned long long)attempt.ahbUsage, ahbRes);
    if (ahbRes != 0 || !attempt.ahb) {
        attempt.failure = "ahb_alloc";
        return attempt;
    }

    VkAndroidHardwareBufferPropertiesANDROID ahbProps{};
    ahbProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    VkAndroidHardwareBufferFormatPropertiesANDROID ahbFormatProps{};
    ahbFormatProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    ahbProps.pNext = &ahbFormatProps;
    VkResult res = g_ahbProps(g_dev, attempt.ahb, &ahbProps);
    PLOG("    vkGetAndroidHardwareBufferPropertiesANDROID -> %s memoryTypeBits=0x%x allocationSize=%llu",
         vkResStr(res), ahbProps.memoryTypeBits, (unsigned long long)ahbProps.allocationSize);
    if (res != VK_SUCCESS) {
        attempt.failure = "ahb_props";
        return attempt;
    }
    PLOG("      formatProps: format=%d externalFormat=%llu features=0x%x samplerYcbcr=0x%x",
         (int)ahbFormatProps.format, (unsigned long long)ahbFormatProps.externalFormat,
         ahbFormatProps.formatFeatures, ahbFormatProps.samplerYcbcrConversionComponents.r);

    VkExternalMemoryImageCreateInfo extImageCi{};
    extImageCi.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    extImageCi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

    VkImageCreateInfo imageCi{};
    imageCi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCi.pNext = &extImageCi;
    imageCi.flags = createFlags;
    imageCi.imageType = VK_IMAGE_TYPE_2D;
    imageCi.format = f.format;
    imageCi.extent = {width, height, 1};
    imageCi.mipLevels = 1;
    imageCi.arrayLayers = 1;
    imageCi.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCi.tiling = tiling;
    imageCi.usage = usage;
    imageCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    res = vkCreateImage(g_dev, &imageCi, nullptr, &attempt.image);
    PLOG("    vkCreateImage -> %s", vkResStr(res));
    if (res != VK_SUCCESS) {
        attempt.failure = "create_image";
        return attempt;
    }

    VkMemoryRequirements memReqs{};
    vkGetImageMemoryRequirements(g_dev, attempt.image, &memReqs);
    PLOG("    memory requirements: size=%llu alignment=%llu memoryTypeBits=0x%x",
         (unsigned long long)memReqs.size, (unsigned long long)memReqs.alignment,
         memReqs.memoryTypeBits);

    uint32_t memoryTypeIndex = ~0u;
    const uint32_t compatible = memReqs.memoryTypeBits & ahbProps.memoryTypeBits;
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(g_phys, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if (compatible & (1u << i)) {
            memoryTypeIndex = i;
            break;
        }
    }
    if (memoryTypeIndex == ~0u) {
        PERR("    no memory type compatible with both image (0x%x) and AHB (0x%x)",
             memReqs.memoryTypeBits, ahbProps.memoryTypeBits);
        attempt.failure = "no_memory_type";
        return attempt;
    }

    VkImportAndroidHardwareBufferInfoANDROID importInfo{};
    importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    importInfo.buffer = attempt.ahb;

    VkMemoryDedicatedAllocateInfo dedicatedInfo{};
    dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicatedInfo.image = attempt.image;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &importInfo;
    allocInfo.allocationSize = ahbProps.allocationSize;
    allocInfo.memoryTypeIndex = memoryTypeIndex;

    // Dedicated allocation is required by many drivers when importing.
    importInfo.pNext = &dedicatedInfo;

    res = vkAllocateMemory(g_dev, &allocInfo, nullptr, &attempt.memory);
    PLOG("    vkAllocateMemory(import AHB, type=%u) -> %s", memoryTypeIndex, vkResStr(res));
    if (res != VK_SUCCESS) {
        attempt.failure = "alloc_memory";
        return attempt;
    }

    res = vkBindImageMemory(g_dev, attempt.image, attempt.memory, 0);
    PLOG("    vkBindImageMemory -> %s", vkResStr(res));
    if (res != VK_SUCCESS) {
        attempt.failure = "bind";
        return attempt;
    }

    VkImageViewCreateInfo viewCi{};
    viewCi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCi.image = attempt.image;
    viewCi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCi.format = f.format;
    viewCi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    res = vkCreateImageView(g_dev, &viewCi, nullptr, &attempt.view);
    PLOG("    vkCreateImageView -> %s", vkResStr(res));
    if (res != VK_SUCCESS) {
        attempt.failure = "view";
        return attempt;
    }

    PLOG("    RESULT: full end-to-end color buffer creation SUCCEEDED");
    return attempt;
}

void cleanupAttempt(ImageAttempt* attempt) {
    if (attempt->view) vkDestroyImageView(g_dev, attempt->view, nullptr);
    if (attempt->memory) vkFreeMemory(g_dev, attempt->memory, nullptr);
    if (attempt->image) vkDestroyImage(g_dev, attempt->image, nullptr);
    if (attempt->ahb) AHardwareBuffer_release(attempt->ahb);
    *attempt = ImageAttempt{};
}

void sectionB() {
    PLOG("");
    PLOG("=== B. gfxstream CyBuffer flow, B8G8R8A8_UNORM 1280x720 ===");

    const FormatEntry* bgra = nullptr;
    const FormatEntry* rgba = nullptr;
    for (const auto& f : kFormats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) bgra = &f;
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM) rgba = &f;
    }
    if (!bgra || !rgba) return;

    VkFormatProperties bgraProps{};
    vkGetPhysicalDeviceFormatProperties(g_phys, VK_FORMAT_B8G8R8A8_UNORM, &bgraProps);
    const VkImageUsageFlags full =
        gfxstreamUsageFlags(bgraProps, VK_IMAGE_TILING_OPTIMAL);
    const VkImageUsageFlags noIa = full & ~VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    const VkImageCreateFlags mutExt = (VkImageCreateFlags)(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                                                           VK_IMAGE_CREATE_EXTENDED_USAGE_BIT);

    const uint32_t w = 1280, h = 720;

    // 1. Exactly what gfxstream does today.
    ImageAttempt a1 = attemptColorBuffer(*bgra, w, h, VK_IMAGE_TILING_OPTIMAL, full, mutExt,
                                         "gfxstream exact (BGRA8, full usage, MUT|EXT)");
    cleanupAttempt(&a1);

    // 2. Same, but drop INPUT_ATTACHMENT (likely the Mali blocker).
    ImageAttempt a2 = attemptColorBuffer(*bgra, w, h, VK_IMAGE_TILING_OPTIMAL, noIa, mutExt,
                                         "BGRA8, usage minus INPUT_ATTACHMENT, MUT|EXT");
    cleanupAttempt(&a2);

    // 3. Drop the create flags as well.
    ImageAttempt a3 = attemptColorBuffer(*bgra, w, h, VK_IMAGE_TILING_OPTIMAL, noIa, 0,
                                         "BGRA8, usage minus INPUT_ATTACHMENT, no create flags");
    cleanupAttempt(&a3);

    // 4. RGBA8 control, gfxstream-equivalent usage.
    VkFormatProperties rgbaProps{};
    vkGetPhysicalDeviceFormatProperties(g_phys, VK_FORMAT_R8G8B8A8_UNORM, &rgbaProps);
    const VkImageUsageFlags rgbaFull = gfxstreamUsageFlags(rgbaProps, VK_IMAGE_TILING_OPTIMAL);
    const VkImageUsageFlags rgbaNoIa = rgbaFull & ~VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    ImageAttempt a4 = attemptColorBuffer(*rgba, w, h, VK_IMAGE_TILING_OPTIMAL, rgbaNoIa, mutExt,
                                         "RGBA8, usage minus INPUT_ATTACHMENT, MUT|EXT");
    cleanupAttempt(&a4);

    // 5. BGRA8 with only what a display color buffer needs.
    ImageAttempt a5 = attemptColorBuffer(
        *bgra, w, h, VK_IMAGE_TILING_OPTIMAL,
        (VkImageUsageFlags)(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT),
        0, "BGRA8, CA|SMP|TSRC|TDST, no create flags");
    cleanupAttempt(&a5);
}

// ---------------------------------------------------------------------------
// Section C: GLES / EGL TextureDraw reproduction
// ---------------------------------------------------------------------------

// Shaders copied verbatim from host/gl/texture_draw.cpp so the compile/link
// behaviour matches gfxstream.
const char* kVertexShaderSource =
    "attribute vec4 position;\n"
    "attribute vec2 inCoord;\n"
    "varying vec2 outCoord;\n"
    "uniform vec2 translation;\n"
    "uniform vec2 scale;\n"
    "uniform vec2 coordTranslation;\n"
    "uniform vec2 coordScale;\n"
    "void main(void) {\n"
    "    gl_Position = position;\n"
    "    gl_Position.xy = (gl_Position.xy + translation) * scale;\n"
    "    outCoord = inCoord * coordScale + coordTranslation;\n"
    "}\n";

const char* kFragmentShaderSource =
    "precision mediump float;\n"
    "varying vec2 outCoord;\n"
    "uniform sampler2D tex;\n"
    "uniform float alpha;\n"
    "uniform int composeMode;\n"
    "uniform vec4 color;\n"
    "uniform mat4 colorTransform;\n"
    "void main(void) {\n"
    "    vec4 t = texture2D(tex, outCoord);\n"
    "    t = colorTransform * t;\n"
    "    gl_FragColor = t * alpha;\n"
    "}\n";

struct GlState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLConfig config = nullptr;
};

const char* glErrStr(GLenum err) {
    switch (err) {
        case GL_NO_ERROR: return "GL_NO_ERROR";
        case GL_INVALID_ENUM: return "GL_INVALID_ENUM(0x500)";
        case GL_INVALID_VALUE: return "GL_INVALID_VALUE(0x501)";
        case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION(0x502)";
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION(0x506)";
        case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY(0x505)";
        default: return "GL_ERROR_OTHER";
    }
}

void clearGlErrors() {
    while (glGetError() != GL_NO_ERROR) {
    }
}

// Runs one TextureDraw init step and reports the first GL error it produces.
#define GL_STEP(desc, expr)                                                  \
    do {                                                                     \
        clearGlErrors();                                                     \
        expr;                                                                \
        GLenum err__ = glGetError();                                         \
        if (err__ != GL_NO_ERROR) {                                          \
            PERR("    [FAIL] %-42s -> %s", desc, glErrStr(err__));           \
            g_firstFailedStep = desc;                                        \
        } else {                                                             \
            PLOG("    [ ok ] %-42s", desc);                                  \
        }                                                                    \
    } while (0)

std::string g_firstFailedStep;

GLuint compileShader(GLenum type, const char* source, const char* label) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_FALSE) {
        char log[512] = {};
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        PERR("    [FAIL] %s compile: %s", label, log);
        return 0;
    }
    PLOG("    [ ok ] %s compile", label);
    return shader;
}

void sectionC() {
    PLOG("");
    PLOG("=== C. GLES / EGL (host renderer) ===");

    GlState gl;

    gl.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (gl.display == EGL_NO_DISPLAY) {
        PERR("C0 eglGetDisplay(EGL_DEFAULT_DISPLAY) failed");
        return;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(gl.display, &major, &minor)) {
        PERR("C0 eglInitialize failed: 0x%x", eglGetError());
        return;
    }
    PLOG("C0 EGL %d.%d vendor=%s version=%s", major, minor,
         eglQueryString(gl.display, EGL_VENDOR), eglQueryString(gl.display, EGL_VERSION));
    PLOG("C0 EGL client APIs=%s", eglQueryString(gl.display, EGL_CLIENT_APIS));

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        PERR("C0 eglBindAPI(EGL_OPENGL_ES_API) failed: 0x%x", eglGetError());
        return;
    }

    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLint numConfigs = 0;
    if (!eglChooseConfig(gl.display, configAttribs, &gl.config, 1, &numConfigs)) {
        PERR("C0 eglChooseConfig failed: 0x%x", eglGetError());
        return;
    }
    PLOG("C0 eglChooseConfig -> %d configs", numConfigs);

    const EGLint pbufferAttribs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    gl.surface = eglCreatePbufferSurface(gl.display, gl.config, pbufferAttribs);
    PLOG("C0 eglCreatePbufferSurface -> %s (err=0x%x)",
         gl.surface == EGL_NO_SURFACE ? "FAILED" : "ok", eglGetError());

    EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    gl.context = eglCreateContext(gl.display, gl.config, EGL_NO_CONTEXT, contextAttribs);
    if (gl.context == EGL_NO_CONTEXT) {
        PERR("C0 eglCreateContext failed: 0x%x", eglGetError());
        return;
    }
    if (!eglMakeCurrent(gl.display, gl.surface, gl.surface, gl.context)) {
        PERR("C0 eglMakeCurrent failed: 0x%x", eglGetError());
        return;
    }

    PLOG("C0 GL_VENDOR=%s", (const char*)glGetString(GL_VENDOR));
    PLOG("C0 GL_RENDERER=%s", (const char*)glGetString(GL_RENDERER));
    PLOG("C0 GL_VERSION=%s", (const char*)glGetString(GL_VERSION));
    PLOG("C0 GL_SHADING_LANGUAGE_VERSION=%s", (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));
    PLOG("C0 GL_EXTENSIONS=%s", (const char*)glGetString(GL_EXTENSIONS));

    PLOG("");
    PLOG("C1 TextureDraw::TextureDraw() call sequence");
    GLuint vertexShader = compileShader(GL_VERTEX_SHADER, kVertexShaderSource, "vertex shader");
    GLuint fragmentShader =
        compileShader(GL_FRAGMENT_SHADER, kFragmentShaderSource, "fragment shader");
    if (!vertexShader || !fragmentShader) {
        PERR("C1 shader compile failed; stopping TextureDraw replication");
        return;
    }

    GLuint program = 0;
    GL_STEP("glCreateProgram", program = glCreateProgram());
    GL_STEP("glAttachShader(vertex)", glAttachShader(program, vertexShader));
    GL_STEP("glAttachShader(fragment)", glAttachShader(program, fragmentShader));
    GL_STEP("glLinkProgram", glLinkProgram(program));

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512] = {};
        glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
        PERR("C1 program link failed: %s", log);
        return;
    }
    PLOG("C1 program link ok");

    GL_STEP("glUseProgram", glUseProgram(program));

    GLint positionSlot = -1, inCoordSlot = -1;
    GL_STEP("glGetAttribLocation(position)", positionSlot = glGetAttribLocation(program, "position"));
    GL_STEP("glEnableVertexAttribArray(position)",
            glEnableVertexAttribArray(positionSlot));
    GL_STEP("glGetAttribLocation(inCoord)", inCoordSlot = glGetAttribLocation(program, "inCoord"));
    GL_STEP("glEnableVertexAttribArray(inCoord)", glEnableVertexAttribArray(inCoordSlot));

    PLOG("C1 slots: position=%d inCoord=%d", positionSlot, inCoordSlot);

    GLint alpha = -1, composeMode = -1, colorLoc = -1, coordTranslation = -1, coordScale = -1,
          scaleSlot = -1, translationSlot = -1, textureSlot = -1, colorTransform = -1;
    GL_STEP("glGetUniformLocation(alpha)", alpha = glGetUniformLocation(program, "alpha"));
    GL_STEP("glGetUniformLocation(composeMode)",
            composeMode = glGetUniformLocation(program, "composeMode"));
    GL_STEP("glGetUniformLocation(color)", colorLoc = glGetUniformLocation(program, "color"));
    GL_STEP("glGetUniformLocation(coordTranslation)",
            coordTranslation = glGetUniformLocation(program, "coordTranslation"));
    GL_STEP("glGetUniformLocation(coordScale)", coordScale = glGetUniformLocation(program, "coordScale"));
    GL_STEP("glGetUniformLocation(scale)", scaleSlot = glGetUniformLocation(program, "scale"));
    GL_STEP("glGetUniformLocation(translation)",
            translationSlot = glGetUniformLocation(program, "translation"));
    GL_STEP("glGetUniformLocation(tex)", textureSlot = glGetUniformLocation(program, "tex"));
    GL_STEP("glGetUniformLocation(colorTransform)",
            colorTransform = glGetUniformLocation(program, "colorTransform"));

    PLOG("C1 uniform locations: alpha=%d composeMode=%d color=%d coordTranslation=%d coordScale=%d "
         "scale=%d translation=%d tex=%d colorTransform=%d",
         alpha, composeMode, colorLoc, coordTranslation, coordScale, scaleSlot, translationSlot,
         textureSlot, colorTransform);

    GL_STEP("glUniform1f(alpha, 1.0)", glUniform1f(alpha, 1.0f));
    GL_STEP("glUniform1i(composeMode, DEVICE)", glUniform1i(composeMode, 2));
    GL_STEP("glUniform2f(translation, 0, 0)", glUniform2f(translationSlot, 0.0f, 0.0f));
    GL_STEP("glUniform2f(scale, 1, 1)", glUniform2f(scaleSlot, 1.0f, 1.0f));
    GL_STEP("glUniform2f(coordTranslation, 0, 0)", glUniform2f(coordTranslation, 0.0f, 0.0f));
    GL_STEP("glUniform2f(coordScale, 1, 1)", glUniform2f(coordScale, 1.0f, 1.0f));

    static const GLfloat kIdentityMatrix[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                                0, 0, 1, 0, 0, 0, 0, 1};
    GL_STEP("glUniformMatrix4fv(colorTransform)",
            glUniformMatrix4fv(colorTransform, 1, GL_FALSE, kIdentityMatrix));

    GLuint vertexBuffer = 0, indexBuffer = 0;
    GL_STEP("glGenBuffers(vertex)", glGenBuffers(1, &vertexBuffer));
    GL_STEP("glBindBuffer(ARRAY_BUFFER)", glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer));
    const GLfloat kVertices[8] = {-1, -1, 0, 0, 1, -1, 1, 0};
    GL_STEP("glBufferData(ARRAY_BUFFER)",
            glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW));
    GL_STEP("glGenBuffers(index)", glGenBuffers(1, &indexBuffer));
    GL_STEP("glBindBuffer(ELEMENT_ARRAY_BUFFER)",
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer));
    const GLushort kIndices[6] = {0, 1, 2, 0, 2, 3};
    GL_STEP("glBufferData(ELEMENT_ARRAY_BUFFER)",
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(kIndices), kIndices, GL_STATIC_DRAW));

    GL_STEP("glUseProgram(0)", glUseProgram(0));
    GL_STEP("glDisableVertexAttribArray(position)",
            glDisableVertexAttribArray(positionSlot));
    GL_STEP("glDisableVertexAttribArray(inCoord)", glDisableVertexAttribArray(inCoordSlot));
    GL_STEP("glBindBuffer(ARRAY_BUFFER, 0)", glBindBuffer(GL_ARRAY_BUFFER, 0));
    GL_STEP("glBindBuffer(ELEMENT_ARRAY_BUFFER, 0)", glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));

    PLOG("C1 first failing step: %s",
         g_firstFailedStep.empty() ? "(none)" : g_firstFailedStep.c_str());

    // Actual draw into an FBO to see whether Mali GLES works at all.
    PLOG("");
    PLOG("C2 draw + readback sanity check");
    GLuint texture = 0;
    GL_STEP("glGenTextures", glGenTextures(1, &texture));
    GL_STEP("glBindTexture", glBindTexture(GL_TEXTURE_2D, texture));
    GL_STEP("glTexImage2D(RGBA8 64x64)",
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
    GL_STEP("glTexParameteri(MIN_FILTER)",
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    GL_STEP("glTexParameteri(MAG_FILTER)",
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    GL_STEP("glTexParameteri(WRAP_S)",
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GL_STEP("glTexParameteri(WRAP_T)",
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));

    GLuint fbo = 0, rbo = 0;
    GL_STEP("glGenFramebuffers", glGenFramebuffers(1, &fbo));
    GL_STEP("glBindFramebuffer", glBindFramebuffer(GL_FRAMEBUFFER, fbo));
    GL_STEP("glGenRenderbuffers", glGenRenderbuffers(1, &rbo));
    GL_STEP("glBindRenderbuffer", glBindRenderbuffer(GL_RENDERBUFFER, rbo));
    GL_STEP("glRenderbufferStorage(RGBA8 64x64)",
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, 64, 64));
    GL_STEP("glFramebufferRenderbuffer",
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo));
    GLenum fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    PLOG("C2 framebuffer status: 0x%x (%s)", fboStatus,
         fboStatus == GL_FRAMEBUFFER_COMPLETE ? "COMPLETE" : "INCOMPLETE");

    if (fboStatus == GL_FRAMEBUFFER_COMPLETE) {
        glViewport(0, 0, 64, 64);
        GL_STEP("glClearColor", glClearColor(0.25f, 0.5f, 0.75f, 1.0f));
        GL_STEP("glClear", glClear(GL_COLOR_BUFFER_BIT));
        unsigned char pixel[4] = {0};
        GL_STEP("glReadPixels", glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel));
        PLOG("C2 readback rgba=(%u,%u,%u,%u)", pixel[0], pixel[1], pixel[2], pixel[3]);
    }

    // AHB -> EGLImage -> GLES texture: the interop path gfxstream uses for
    // zero-copy composition (glInteropSupported was false in the device log).
    PLOG("");
    PLOG("C3 AHB -> EGLImage -> GLES texture interop");
    auto eglGetNativeClientBuffer =
        (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    auto eglCreateImageKHR =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PLOG("C3 eglGetNativeClientBufferANDROID: %s", eglGetNativeClientBuffer ? "present" : "MISSING");
    PLOG("C3 eglCreateImageKHR: %s", eglCreateImageKHR ? "present" : "MISSING");

    const bool hasOesEglImage =
        strstr((const char*)glGetString(GL_EXTENSIONS), "GL_OES_EGL_image") != nullptr;
    PLOG("C3 GL_OES_EGL_image: %s", hasOesEglImage ? "present" : "MISSING");

    if (eglGetNativeClientBuffer && eglCreateImageKHR && hasOesEglImage) {
        AHardwareBuffer* ahb = nullptr;
        AHardwareBuffer_Desc desc{};
        desc.width = 64;
        desc.height = 64;
        desc.layers = 1;
        desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                     AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
        int ahbRes = AHardwareBuffer_allocate(&desc, &ahb);
        PLOG("C3 AHardwareBuffer_allocate -> %d", ahbRes);
        if (ahbRes == 0 && ahb) {
            EGLClientBuffer clientBuffer = eglGetNativeClientBuffer(ahb);
            PLOG("C3 eglGetNativeClientBuffer -> %p", clientBuffer);
            if (clientBuffer) {
                const EGLint imageAttribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
                EGLImageKHR image = eglCreateImageKHR(gl.display, EGL_NO_CONTEXT,
                                                      EGL_NATIVE_BUFFER_ANDROID, clientBuffer,
                                                      imageAttribs);
                PLOG("C3 eglCreateImageKHR -> %p (err=0x%x)", image, eglGetError());
                if (image != EGL_NO_IMAGE_KHR) {
                    GLuint tex = 0;
                    GL_STEP("glGenTextures(ahb)", glGenTextures(1, &tex));
                    GL_STEP("glBindTexture(ahb)", glBindTexture(GL_TEXTURE_2D, tex));
                    auto glEGLImageTargetTexture2DOESFunc =
                        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
                            "glEGLImageTargetTexture2DOES");
                    PLOG("C3 glEGLImageTargetTexture2DOES: %s",
                         glEGLImageTargetTexture2DOESFunc ? "present" : "MISSING");
                    if (glEGLImageTargetTexture2DOESFunc) {
                        GL_STEP("glEGLImageTargetTexture2DOES",
                                glEGLImageTargetTexture2DOESFunc(GL_TEXTURE_2D, image));
                    }
                    eglDestroyImage(gl.display, image);
                }
            }
            AHardwareBuffer_release(ahb);
        }
    }

    PLOG("");
    PLOG("=== C done: first failing TextureDraw step: %s ===",
         g_firstFailedStep.empty() ? "(none)" : g_firstFailedStep.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    PLOG("MALI PROBE v1 (%s)", __DATE__);
    if (!initVulkan()) {
        PERR("Vulkan init failed; continuing with GLES only");
    } else {
        sectionA();
        sectionB();
    }
    sectionC();
    PLOG("");
    PLOG("MALI PROBE done");

    if (g_dev != VK_NULL_HANDLE) vkDestroyDevice(g_dev, nullptr);
    if (g_instance != VK_NULL_HANDLE) vkDestroyInstance(g_instance, nullptr);
    return 0;
}
