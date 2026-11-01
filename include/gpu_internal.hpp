#pragma once
// Helpers shared by src/gpu_frames.cpp and src/optical_flow.cpp (command buffers of the GpuState, barriers, memory, images).

#include "gpu_frames.hpp"

namespace gpuint {

void Barrier(const DeviceDispatch& dt, VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
             VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);
// A global memory dependency (all images stay in GENERAL during compute passes).
void MemBarrier(const DeviceDispatch& dt, VkCommandBuffer cb, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);
uint32_t FindMemoryType(const DeviceDispatch& dt, uint32_t bits, VkMemoryPropertyFlags want);
// Takes the next command buffer, waiting for its previous submission to finish, and starts recording.
bool BeginCmd(const DeviceDispatch& dt, GpuState& g, uint32_t& entry);
// Ends and submits the command buffer; `signal` may be VK_NULL_HANDLE (nothing is signaled).
bool EndAndSubmit(const DeviceDispatch& dt, VkQueue queue, GpuState& g, uint32_t entry,
                  const std::vector<VkSemaphoreSubmitInfo>& waits, VkSemaphore signal);
// A device-local 2D storage image (usable from several queue families when the device has them).
bool CreateStorageImage(const DeviceDispatch& dt, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, FlowImage& out);
void DestroyFlowImage(const DeviceDispatch& dt, FlowImage& img);

// Debug: copies an image (in GENERAL layout) to a host buffer and writes it to `path` (raw texels). Caller holds g.gpu_mu.
// VKIFRAME_DUMP: reads the image back into host memory (g.dump) with a short GPU wait and no file write; g.dump.Flush writes the queued files.
bool DumpImage(const DeviceDispatch& dt, VkQueue queue, GpuState& g, VkImage image, VkExtent2D extent, uint32_t bytes_per_texel, const char* path);

} // namespace gpuint
