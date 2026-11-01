// Flow images with the optional TRANSFER_SRC usage (VKIFRAME_DUMP copies the final flow and the mask): kept when the format and the driver allow it,
// dropped with a log line when they do not, never added to images that did not ask for it. Fake driver, no GPU.
#include "gpu_internal.hpp"

#include <cstdio>
#include <cstdlib>

using namespace gpuint;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static bool f_format_has_transfer_src, f_driver_refuses_flag, f_driver_refuses_all, f_alloc_fails;
static int  f_creates, f_with_flag, f_views, f_images_live, f_mems_live, f_format_queries;
static VkImageUsageFlags f_last_usage;
static VkImage g_img = (VkImage)(uintptr_t)0x10;

static void VKAPI_CALL F_FormatProps(VkPhysicalDevice, VkFormat, VkFormatProperties* fp) {
    ++f_format_queries; fp->optimalTilingFeatures = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | (f_format_has_transfer_src ? VK_FORMAT_FEATURE_TRANSFER_SRC_BIT : 0);
}
static void VKAPI_CALL F_MemProps(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* mp) { mp->memoryTypeCount = 1; mp->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT; }
static VkResult VKAPI_CALL F_CreateImage(VkDevice, const VkImageCreateInfo* ci, const VkAllocationCallbacks*, VkImage* out) {
    ++f_creates; f_last_usage = ci->usage; const bool flag = ci->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT; if (flag) ++f_with_flag;
    if (f_driver_refuses_all || (flag && f_driver_refuses_flag)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    *out = g_img; ++f_images_live; return VK_SUCCESS;
}
static void VKAPI_CALL F_DestroyImage(VkDevice, VkImage, const VkAllocationCallbacks*) { --f_images_live; }
static void VKAPI_CALL F_GetReq(VkDevice, VkImage, VkMemoryRequirements* r) { r->size = 4096; r->alignment = 1; r->memoryTypeBits = 1; }
static VkResult VKAPI_CALL F_Alloc(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory* m) { if (f_alloc_fails) return VK_ERROR_OUT_OF_DEVICE_MEMORY; *m = (VkDeviceMemory)(uintptr_t)0x20; ++f_mems_live; return VK_SUCCESS; }
static void VKAPI_CALL F_Free(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) { --f_mems_live; }
static VkResult VKAPI_CALL F_Bind(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
static VkResult VKAPI_CALL F_View(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* v) { *v = (VkImageView)(uintptr_t)0x30; ++f_views; return VK_SUCCESS; }
static void VKAPI_CALL F_DestroyView(VkDevice, VkImageView, const VkAllocationCallbacks*) { --f_views; }

static void Reset() { f_format_has_transfer_src = true; f_driver_refuses_flag = f_driver_refuses_all = f_alloc_fails = false; f_creates = f_with_flag = f_views = f_images_live = f_mems_live = f_format_queries = 0; f_last_usage = 0; }

int main() {
    InstanceDispatch inst; inst.GetPhysicalDeviceFormatProperties = F_FormatProps; inst.GetPhysicalDeviceMemoryProperties = F_MemProps;
    DeviceDispatch dt; dt.instance = &inst;
    dt.CreateImage = F_CreateImage; dt.DestroyImage = F_DestroyImage; dt.GetImageMemoryRequirements = F_GetReq; dt.AllocateMemory = F_Alloc; dt.FreeMemory = F_Free;
    dt.BindImageMemory = F_Bind; dt.CreateImageView = F_View; dt.DestroyImageView = F_DestroyView;
    std::freopen("/dev/null", "w", stderr);
    const VkExtent2D ext{320, 180};
    const VkImageUsageFlags kStorage = VK_IMAGE_USAGE_STORAGE_BIT, kDump = kStorage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    {   // supported: the flag is kept
        Reset(); FlowImage im;
        CHECK(CreateStorageImage(dt, VK_FORMAT_R16G16B16A16_SFLOAT, ext, kDump, im) && im.transfer_src && (f_last_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && f_creates == 1);
        DestroyFlowImage(dt, im); CHECK(f_images_live == 0 && f_mems_live == 0 && f_views == 0);
    }
    {   // an image that does not ask for the flag never gets it, and the format is not even queried
        Reset(); FlowImage im;
        CHECK(CreateStorageImage(dt, VK_FORMAT_R32_SFLOAT, ext, kStorage, im) && !im.transfer_src && f_last_usage == kStorage && f_format_queries == 0);
        DestroyFlowImage(dt, im);
    }
    {   // the format does not list the feature: created without the flag, and the driver is not even asked with it
        Reset(); f_format_has_transfer_src = false; FlowImage im;
        CHECK(CreateStorageImage(dt, VK_FORMAT_R16G16B16A16_SFLOAT, ext, kDump, im) && !im.transfer_src && f_with_flag == 0 && f_last_usage == kStorage);
        DestroyFlowImage(dt, im); CHECK(f_images_live == 0 && f_mems_live == 0);
    }
    {   // the driver refuses the image with the flag: one retry without it, the engine's image exists
        Reset(); f_driver_refuses_flag = true; FlowImage im;
        CHECK(CreateStorageImage(dt, VK_FORMAT_R16G16B16A16_SFLOAT, ext, kDump, im) && !im.transfer_src && f_creates == 2 && f_with_flag == 1 && f_last_usage == kStorage);
        DestroyFlowImage(dt, im); CHECK(f_images_live == 0 && f_mems_live == 0 && f_views == 0);
    }
    {   // the driver refuses even the plain image: false, and no retry loop (a request without the flag is not retried)
        Reset(); f_driver_refuses_all = true; FlowImage a, b;
        CHECK(!CreateStorageImage(dt, VK_FORMAT_R32_SFLOAT, ext, kStorage, a) && f_creates == 1);
        Reset(); f_driver_refuses_all = true;
        CHECK(!CreateStorageImage(dt, VK_FORMAT_R16G16B16A16_SFLOAT, ext, kDump, b) && f_creates == 2 && !b.transfer_src);
    }
    {   // memory allocation fails: false and the image can be destroyed by the caller
        Reset(); f_alloc_fails = true; FlowImage im;
        CHECK(!CreateStorageImage(dt, VK_FORMAT_R16G16B16A16_SFLOAT, ext, kDump, im));
        DestroyFlowImage(dt, im); CHECK(f_images_live == 0 && f_mems_live == 0);
    }
    std::printf(g_fail ? "FAILED\n" : "flow images: ok\n");
    return g_fail ? 1 : 0;
}
