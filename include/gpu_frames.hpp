#pragma once
#include <atomic>

// Stage 3b: GPU side of holding a frame. The game's image is blitted into an internal RGBA16F ring slot ("captured"),
// the real image is held back (acquired, not presented) and presented one native frame later. Every Vulkan call
// goes through the DeviceDispatch table. All ring images stay in VK_IMAGE_LAYOUT_GENERAL.

#include "vk_iframes_layer.hpp"

#include <condition_variable>
#include <mutex>

struct HeldFrame {
    bool     valid{false};
    uint32_t image_index{0};    // the game's real swapchain image
    uint32_t slot{0};           // ring slot holding its copy
    VkPresentModeKHR mode{};     // the application's per-present mode (VkSwapchainPresentModeInfoKHR), when has_mode
    bool     has_mode{false};
    VkFence  fence{};           // the application's VK_EXT/KHR_swapchain_maintenance1 present fence for this frame (signaled when it is shown)
    uint64_t present_id{0};     // the application's VK_KHR_present_id value for this frame (0 = none), re-attached when it is shown
};

// The most command buffer entries a swapchain can use (the live CMD_BUFFERS setting picks how many, default 8). Each entry has its own fence, descriptor sets and query slots.
constexpr uint32_t kMaxCmdBuffers = 32;

// An image of the optical-flow engine (Stage 4).
struct FlowImage { VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; VkExtent2D extent{}; bool initialized{false}; };

// Stage 4: pipelines, images and descriptor sets of the optical-flow engine (src/optical_flow.cpp).
struct FlowState {
    static constexpr uint32_t kLevels = 4;           // pyramid levels 1..4 (1/2 .. 1/16); flow is estimated at level 1 (half resolution)
    static constexpr uint32_t kPasses = 19;          // descriptor sets per command buffer entry (see optical_flow.cpp)
    struct Pass { VkShaderModule shader{}; VkDescriptorSetLayout set_layout{}; VkPipelineLayout layout{}; VkPipeline pipeline{}; };
    bool ready{false};                               // pipelines and images exist
    bool failed{false};                              // a runtime failure switched the engine off for this swapchain
    uint32_t fin{0};                                 // pyramid index of the finished flow field (Config::flow_level - 1)
    bool pair_bidir{false};                          // INTERP_BIDIR as it was when this pair's flow was computed (the setting can change while a game runs)
    bool pair_valid{false};                          // flow, backward flow and mask of the current pair are in the images below
    Pass luma, down, coarse, refine, occlusion, vis, interp;
    FlowImage pyr[2][kLevels];                       // [0] = frame A, [1] = frame B; index = level - 1; R32F luma
    FlowImage fwd[kLevels], bwd[kLevels];            // RGBA16F (x, y, cost, 0); fwd[fin] is the final field, A -> B
    FlowImage mask;                                  // R32F, allocated at level 1 size; the part covering fwd[fin] is used: forward/backward disagreement
    VkDescriptorPool pool{};
    VkDescriptorSet  sets[kMaxCmdBuffers][kPasses]{};   // one row per command buffer entry (GpuState::n_cmd of them are used)
    // GPU time of the engine (timestamp queries, read back when a command buffer entry is reused)
    VkQueryPool qpool{};
    float       ts_period_ns{0.0f};
    static constexpr uint32_t kQSlots = 10;          // timestamps per command buffer entry: 0..7 flow stage boundaries, 8..9 the interpolation pass
    bool        ts_pending[kMaxCmdBuffers]{};
    uint32_t    ts_n{0};                             // stamps written by one flow pass: start, pyramid, coarse, one per refine level, end
    double      stage_sum[8]{};                      // per interval between stamps
    double      ms_sum{0.0}, ms_max{0.0};
    uint32_t    ms_count{0};
    bool        interp_pending[kMaxCmdBuffers]{};
    double      interp_sum{0.0}, interp_max{0.0};
    uint32_t    interp_count{0};
    // GPU time of the current second (read and cleared by LogRates): feeds the cost warning and VKIFRAME_STATS; two atomic adds per pair / generated frame
    std::atomic<uint64_t> stat_flow_ns{0}, stat_flow_n{0}, stat_interp_ns{0}, stat_interp_n{0};
};

struct GpuState {
    static constexpr VkFormat kRingFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr uint32_t kRingCapacity = 6;     // frames waiting for their ticks (held, queued jobs) plus the one being captured
    static constexpr uint32_t kCmdBuffers = kMaxCmdBuffers;   // array size; n_cmd of them are used
    uint32_t n_cmd{8};                               // entries allocated (all kMaxCmdBuffers); the live CMD_BUFFERS setting picks how many are used: the more entries, the further the layer may run ahead of the GPU before the game thread waits on a fence

    struct Slot { VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; bool initialized{false}; };

    VkExtent2D extent{};
    uint32_t   family{0};
    std::vector<VkImage>     swap_images;
    Slot                     ring[kRingCapacity];
    Slot                     synth;                  // one output image is enough: jobs run in queue order and each is blitted out right away
    VkSwapchainKHR           swapchain{};
    // compute pass for the synthetic frame (Stage 3: blend.comp)
    bool                     synth_ready{false};
    VkShaderModule           shader{};
    VkDescriptorSetLayout    set_layout{};
    VkPipelineLayout         pipe_layout{};
    VkPipeline               pipeline{};
    VkDescriptorPool         desc_pool{};
    VkDescriptorSet          sets[kCmdBuffers]{};    // one per command buffer entry: its fence guards the update
    std::vector<VkSemaphore> acquire;                // per command buffer entry: waited by the submit that blits into a hidden image
    uint32_t                 ring_write{0};
    VkCommandPool            pool{};
    VkCommandBuffer          cbs[kCmdBuffers]{};
    VkFence                  fences[kCmdBuffers]{};
    uint32_t                 cb_next{0};
    std::vector<VkSemaphore> captured;     // per swapchain image: signaled by the capture submit
    std::vector<VkSemaphore> staged;       // per swapchain image: signaled by the debug round trip
    FlowState                flow;
    uint64_t                 pairs_flowed{0};        // pairs that went through GpuFlow (VKIFRAME_DUMP counts them)
    int                      dump_pair_index{0}, dump_tick{0};   // VKIFRAME_DUMP naming
    uint64_t                 mark_tick{0}, mark_pair{0};   // VKIFRAME_TICKMARK bar position counters (ticks are synthesized one at a time)
    float                    mark_last_t{-1.0f};
    int                      dump_synth{0};          // synthetic frames still to dump after the dumped pair
    HeldFrame                held;
    HeldFrame                last;                   // VKIFRAME_EXTRAPOLATE: the previous native frame (ring copy only, its real image has been presented); one ring reference
    // where the game thread waits (ns, window totals; read and reset by LogRates): gpu_mu, a free ring slot, a command buffer's fence
    std::atomic<uint64_t>    wait_lock_ns{0}, wait_ring_ns{0}, wait_fence_ns{0};
    // Stage 3c-ii: the game thread captures while the presenter thread synthesizes.
    std::mutex               gpu_mu;                 // command buffers, fences, ring selection, descriptor sets
    std::mutex               acquire_mu;             // vkAcquireNextImageKHR on this swapchain (ours and the game's)
    std::condition_variable  pin_cv;
    int                      pins[kRingCapacity]{};  // references to a ring slot: held frame, queued jobs; a pinned slot is never overwritten
};

// Reference counting of ring slots. GpuCapture returns with the new slot pinned once (the "held" reference).
void GpuPin(GpuState& g, uint32_t slot);
void GpuUnpin(GpuState& g, uint32_t slot);

// Creates the ring, command buffers, fences and semaphores for a swapchain. `queue_family` is the family of the present queue.
GpuState* GpuInit(const DeviceDispatch& dt, SwapchainState& sc, uint32_t queue_family);

// Records and submits the capture of the game's image (pi->pImageIndices[0]). Waits on the game's wait semaphores.
// On success `out` describes the held frame; the real image is NOT presented.
bool GpuCapture(const DeviceDispatch& dt, VkQueue queue, GpuState& g, const VkPresentInfoKHR& pi, HeldFrame& out);

// Returns the semaphore the present of `h` must wait on. With `via_ring` the held image is first overwritten with the
// ring copy (a visible proof that the capture is correct); false means that step failed.
bool GpuPrepareHeldPresent(const DeviceDispatch& dt, VkQueue queue, GpuState& g, const HeldFrame& h, bool via_ring, VkSemaphore& wait_sem);

// Stage 4 (src/optical_flow.cpp). Builds the pipelines and images (false: the engine stays off, synthetic frames are unaffected).
bool FlowCreate(const DeviceDispatch& dt, GpuState& g);
void FlowDestroy(const DeviceDispatch& dt, GpuState& g);
// Estimates the flow A -> B and B -> A and the occlusion mask of a pair. Runs on `queue` (ordered after the captures of both
// frames by barriers). Sets g.flow.pair_valid; on failure the engine is switched off.
bool GpuFlow(const DeviceDispatch& dt, VkQueue queue, GpuState& g, const HeldFrame& a, const HeldFrame& b);

// Frees everything (waits for the device to be idle first).
void GpuDestroy(const DeviceDispatch& dt, GpuState* g);

// Builds frame(t) = mix(A, B, t) with the compute pass, blits it into a swapchain image and returns the semaphore the
// present of that image must wait on. hidden == false: the target is the real image of A (the carrier of the first tick,
// A's own copy stays in the ring). hidden == true: the target is a freshly acquired hidden image. Returns VK_SUCCESS or
// the Vulkan error (VK_ERROR_OUT_OF_DATE_KHR from the acquire is forwarded).
VkResult GpuSynthesize(const DeviceDispatch& dt, VkQueue queue, GpuState& g, const HeldFrame& a, const HeldFrame& b, float t,
                       bool hidden, uint32_t& out_image_index, VkSemaphore& wait_sem, bool extrap = false);
