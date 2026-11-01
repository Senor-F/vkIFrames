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

// VKIFRAME_DUMP. DumpInit allocates the pool (at start, after the flow engine exists). DumpRecord records the copy of `image` into the next
// free pool buffer inside `cb` (no wait, no CPU work) and returns it, or null when the pool is not ready or too small. DumpFlush waits once for
// every submitted copy, writes the files from the mapped memory and frees the pool. DumpTeardown (GpuDestroy) logs what is dropped and frees.
void DumpInit(const DeviceDispatch& dt, GpuState& g);
DumpPool::Buf* DumpRecord(const DeviceDispatch& dt, VkCommandBuffer cb, VkImage image, VkExtent2D extent, uint32_t bytes_per_texel, DumpPool::Buf* buf, const char* name);
void DumpFlush(const DeviceDispatch& dt, GpuState& g);
void DumpMaybeFlush(const DeviceDispatch& dt, GpuState& g, bool force);
void DumpTeardown(const DeviceDispatch& dt, GpuState& g);

} // namespace gpuint
