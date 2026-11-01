// Images of the optical-flow engine and the memory-type lookup they need (kept apart so a mock test can link them without the rest of the layer).
#include "gpu_internal.hpp"

namespace gpuint {

uint32_t FindMemoryType(const DeviceDispatch& dt, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    dt.instance->GetPhysicalDeviceMemoryProperties(dt.physical_device, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

// `usage` may include VK_IMAGE_USAGE_TRANSFER_SRC_BIT as an optional extra (the VKIFRAME_DUMP copies of the final flow and the mask): it is dropped, with a log
// line, when the format does not list the transfer-source feature or the driver refuses the image with it. out.transfer_src says whether it was kept.
bool CreateStorageImage(const DeviceDispatch& dt, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, FlowImage& out) {
    out.transfer_src = false;
    VkImageUsageFlags use = usage;
    if (use & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
        VkFormatProperties fp{};
        if (dt.instance && dt.instance->GetPhysicalDeviceFormatProperties) dt.instance->GetPhysicalDeviceFormatProperties(dt.physical_device, format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) {
            use &= ~VkImageUsageFlags(VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            FG_LOG("dump: format %d does not support transfer-source use; this image will not be dumped", int(format));
        }
    }
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = format;
    ci.extent = {extent.width, extent.height, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = use;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;                  // only the present queue's family uses these
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    out.extent = extent;
    if (!dt.CreateImage || !dt.CreateImageView) return false;
    VkResult r = dt.CreateImage(dt.device, &ci, nullptr, &out.image);
    if (r != VK_SUCCESS && (use & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {      // the optional flag only: try again without it
        FG_LOG("dump: the driver refused transfer-source use on format %d (%d); this image will not be dumped", int(format), int(r));
        out.image = VK_NULL_HANDLE;
        use &= ~VkImageUsageFlags(VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        ci.usage = use;
        r = dt.CreateImage(dt.device, &ci, nullptr, &out.image);
    }
    if (r != VK_SUCCESS) { out.image = VK_NULL_HANDLE; return false; }
    out.transfer_src = (use & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    VkMemoryRequirements req; dt.GetImageMemoryRequirements(dt.device, out.image, &req);
    const uint32_t type = FindMemoryType(dt, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, type};
    if (type == UINT32_MAX || dt.AllocateMemory(dt.device, &ai, nullptr, &out.memory) != VK_SUCCESS ||
        dt.BindImageMemory(dt.device, out.image, out.memory, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return dt.CreateImageView(dt.device, &vi, nullptr, &out.view) == VK_SUCCESS;
}

void DestroyFlowImage(const DeviceDispatch& dt, FlowImage& img) {
    if (img.view && dt.DestroyImageView) dt.DestroyImageView(dt.device, img.view, nullptr);
    if (img.image)  dt.DestroyImage(dt.device, img.image, nullptr);
    if (img.memory) dt.FreeMemory(dt.device, img.memory, nullptr);
    img = {};
}

} // namespace gpuint
