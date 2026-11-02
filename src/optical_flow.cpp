// Stage 4: hierarchical optical flow between the two frames of a pair (A = older, B = newer), computed on the present queue.
//
// Per pair (all passes in one command buffer, every image stays in GENERAL):
//   luma of A and B at half resolution (level 1), then levels 2..4 by 2x2 averages            [flow_luma, flow_down]
//   exhaustive block search at level 4, then per-level refinement 4 -> 3 -> 2 -> 1            [flow_coarse, flow_refine]
//   the same chain with A and B swapped (backward flow)
//   forward/backward consistency at level 1                                                   [flow_occlusion]
// The algorithm is include/flow_ref.hpp (tested against known motion); the shaders mirror it. Vectors are in pixels of their own
// level; the final field is at the level of Config::flow_level (default 2, quarter resolution): multiply by 2^level for full-resolution pixels.
//
// Descriptor sets: kPasses sets per command buffer entry, in this order:
//   0 luma A | 1 luma B | 2-4 down A (levels 2..4) | 5-7 down B | 8 coarse fwd | 9 coarse bwd | 10-12 refine fwd (levels 3,2,1)
//   13-15 refine bwd | 16 occlusion | 17 debug view | 18 interpolation (17 and 18 are bound by GpuSynthesize)

#include "gpu_internal.hpp"
#include "live_config.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include "flow_luma_spv.h"
#include "flow_down_spv.h"
#include "flow_coarse_spv.h"
#include "flow_refine_spv.h"
#include "flow_occlusion_spv.h"
#include "flow_vis_spv.h"
#include "flow_interp_spv.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <initializer_list>
#include <mutex>
#include <vector>

using namespace gpuint;

namespace {

// must match include/flow_ref.hpp (kPatch, kRadius, kOcclThreshold)
constexpr int   kPatch[FlowState::kLevels]  = {1, 2, 3, 3};
constexpr int   kRadius[FlowState::kLevels] = {1, 1, 2, 2};
constexpr float kOcclThreshold = 1.5f;
// With the finest level at 1 the backward flow can stop one level short (VKIFRAME_FLOW_BWD_SKIP): the mask then reads the coarser field,
// whose whole-pixel vectors double to even numbers, so the threshold gets a pixel of slack. Not with the candidate search, which reads the finest backward field.
bool SkipFinestBackward(const FlowState& f) {
    const int mode = GetConfig().flow_bwd_skip;
    return !f.pair_bidir && !GetConfig().extrapolate && GetConfig().cand_thr <= 0.0f && f.fin + 1 < FlowState::kLevels && ((mode >= 1 && f.fin == 0) || mode >= 2);
}

static_assert(GpuState::kCmdBuffers == kMaxCmdBuffers, "FlowState::sets has one row per command buffer entry");

struct PassSpec { FlowState::Pass FlowState::* member; const uint32_t* spv; size_t bytes; uint32_t bindings; uint32_t push_bytes; const char* name; };

// which pass each of the kPasses sets belongs to
FlowState::Pass FlowState::* const kSetPass[FlowState::kPasses] = {
    &FlowState::luma, &FlowState::luma,
    &FlowState::down, &FlowState::down, &FlowState::down, &FlowState::down, &FlowState::down, &FlowState::down,
    &FlowState::coarse, &FlowState::coarse,
    &FlowState::refine, &FlowState::refine, &FlowState::refine, &FlowState::refine, &FlowState::refine, &FlowState::refine,
    &FlowState::occlusion, &FlowState::vis, &FlowState::interp};
constexpr uint32_t kSetBindings[FlowState::kPasses] = {2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 4, 4, 4, 4, 4, 4, 3, 4, 6};
constexpr uint32_t kSetsStorageImages = 2 * 2 + 6 * 2 + 2 * 3 + 6 * 4 + 3 + 4 + 6;       // per command buffer entry

bool CreatePass(const DeviceDispatch& dt, FlowState::Pass& p, const PassSpec& s) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = s.bytes; mi.pCode = s.spv;
    if (dt.CreateShaderModule(dt.device, &mi, nullptr, &p.shader) != VK_SUCCESS) { FG_LOG("flow: the driver rejected %s", s.name); return false; }
    VkDescriptorSetLayoutBinding b[8]{};
    for (uint32_t i = 0; i < s.bindings; ++i) { b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = s.bindings; li.pBindings = b;
    if (dt.CreateDescriptorSetLayout(dt.device, &li, nullptr, &p.set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, s.push_bytes};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1; pl.pSetLayouts = &p.set_layout; pl.pushConstantRangeCount = s.push_bytes ? 1 : 0; pl.pPushConstantRanges = s.push_bytes ? &pc : nullptr;
    if (dt.CreatePipelineLayout(dt.device, &pl, nullptr, &p.layout) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, p.shader, "main", nullptr};
    pi.layout = p.layout;
    if (dt.CreateComputePipelines(dt.device, VK_NULL_HANDLE, 1, &pi, nullptr, &p.pipeline) != VK_SUCCESS) { FG_LOG("flow: pipeline creation failed (%s)", s.name); return false; }
    return true;
}

void DestroyPass(const DeviceDispatch& dt, FlowState::Pass& p) {
    if (p.pipeline)   dt.DestroyPipeline(dt.device, p.pipeline, nullptr);
    if (p.layout)     dt.DestroyPipelineLayout(dt.device, p.layout, nullptr);
    if (p.set_layout) dt.DestroyDescriptorSetLayout(dt.device, p.set_layout, nullptr);
    if (p.shader)     dt.DestroyShaderModule(dt.device, p.shader, nullptr);
    p = {};
}

VkExtent2D HalfUp(VkExtent2D e) { return {(e.width + 1) / 2, (e.height + 1) / 2}; }

} // namespace

void TimingInit(const DeviceDispatch& dt, GpuState& g) {
    FlowState& f = g.flow;
    if (f.qpool) return;
    // GPU timestamps (optional): the present queue's family must support them
    if (dt.CreateQueryPool && dt.DestroyQueryPool && dt.CmdResetQueryPool && dt.CmdWriteTimestamp && dt.GetQueryPoolResults &&
        dt.instance->GetPhysicalDeviceProperties && dt.instance->GetPhysicalDeviceQueueFamilyProperties) {
        VkQueueFamilyProperties qf[16]{}; uint32_t nq = 16;
        dt.instance->GetPhysicalDeviceQueueFamilyProperties(dt.physical_device, &nq, qf);
        VkPhysicalDeviceProperties props{};
        dt.instance->GetPhysicalDeviceProperties(dt.physical_device, &props);
        if (g.family < nq && qf[g.family].timestampValidBits >= 40 && props.limits.timestampPeriod > 0.0f) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = FlowState::kQSlots * g.n_cmd;
            if (dt.CreateQueryPool(dt.device, &qi, nullptr, &f.qpool) == VK_SUCCESS) f.ts_period_ns = props.limits.timestampPeriod;
        }
    }
}

bool FlowCreate(const DeviceDispatch& dt, GpuState& g) {
    FlowState& f = g.flow;
    f.fin = uint32_t(std::clamp(GetConfig().flow_level, 1, int(FlowState::kLevels) - 1) - 1);
    const bool have = dt.CreateShaderModule && dt.DestroyShaderModule && dt.CreateDescriptorSetLayout && dt.DestroyDescriptorSetLayout &&
                      dt.CreatePipelineLayout && dt.DestroyPipelineLayout && dt.CreateComputePipelines && dt.DestroyPipeline &&
                      dt.CreateDescriptorPool && dt.DestroyDescriptorPool && dt.AllocateDescriptorSets && dt.UpdateDescriptorSets &&
                      dt.CmdBindPipeline && dt.CmdBindDescriptorSets && dt.CmdPushConstants && dt.CmdDispatch &&
                      dt.CreateImage && dt.DestroyImage && dt.CreateImageView && dt.DestroyImageView;
    if (!have) { FG_LOG("flow: the driver does not expose the compute functions"); return false; }
    for (const auto& s : {sizeof(kFlowLumaSpv), sizeof(kFlowDownSpv), sizeof(kFlowCoarseSpv), sizeof(kFlowRefineSpv), sizeof(kFlowOcclusionSpv), sizeof(kFlowVisSpv), sizeof(kFlowInterpSpv)})
        if (s < 256) { FG_LOG("flow: shaders not built into this library"); return false; }

    const PassSpec specs[] = {
        {&FlowState::luma,      kFlowLumaSpv,      sizeof(kFlowLumaSpv),      2, 0,                       "flow_luma"},
        {&FlowState::down,      kFlowDownSpv,      sizeof(kFlowDownSpv),      2, 0,                       "flow_down"},
        {&FlowState::coarse,    kFlowCoarseSpv,    sizeof(kFlowCoarseSpv),    3, sizeof(int),             "flow_coarse"},
        {&FlowState::refine,    kFlowRefineSpv,    sizeof(kFlowRefineSpv),    4, 3 * sizeof(int),         "flow_refine"},
        {&FlowState::occlusion, kFlowOcclusionSpv, sizeof(kFlowOcclusionSpv), 3, 2 * sizeof(float),       "flow_occlusion"},
        {&FlowState::vis,       kFlowVisSpv,       sizeof(kFlowVisSpv),       4, 0,                       "flow_vis"},
        {&FlowState::interp,    kFlowInterpSpv,      sizeof(kFlowInterpSpv),      6, 11 * sizeof(float),      "flow_interp"},
    };
    for (const auto& s : specs) if (!CreatePass(dt, f.*s.member, s)) return false;

    // images: luma pyramid of A and B (R32F), forward and backward flow per level (RGBA16F), mask at level 1 (R32F)
    constexpr VkImageUsageFlags kUse = VK_IMAGE_USAGE_STORAGE_BIT;
    // VKIFRAME_DUMP copies the final forward and backward flow and the mask to files: only those three get TRANSFER_SRC, and only then (the pyramid and the other levels never do)
    const VkImageUsageFlags kDump = kUse | (GetConfig().dump_pair > 0 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    VkExtent2D ext[FlowState::kLevels];
    ext[0] = HalfUp(g.extent);
    for (uint32_t i = 1; i < FlowState::kLevels; ++i) ext[i] = HalfUp(ext[i - 1]);
    for (uint32_t i = 0; i < FlowState::kLevels; ++i) {
        if (!CreateStorageImage(dt, VK_FORMAT_R32_SFLOAT, ext[i], kUse, f.pyr[0][i]) || !CreateStorageImage(dt, VK_FORMAT_R32_SFLOAT, ext[i], kUse, f.pyr[1][i]) ||
            !CreateStorageImage(dt, VK_FORMAT_R16G16B16A16_SFLOAT, ext[i], i == f.fin ? kDump : kUse, f.fwd[i]) || !CreateStorageImage(dt, VK_FORMAT_R16G16B16A16_SFLOAT, ext[i], i == f.fin ? kDump : kUse, f.bwd[i])) return false;
    }
    if (!CreateStorageImage(dt, VK_FORMAT_R32_SFLOAT, ext[0], kDump, f.mask)) return false;

    // descriptor sets
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSetsStorageImages * g.n_cmd};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = FlowState::kPasses * g.n_cmd; dp.poolSizeCount = 1; dp.pPoolSizes = &ps;
    if (dt.CreateDescriptorPool(dt.device, &dp, nullptr, &f.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[FlowState::kPasses];
    for (uint32_t i = 0; i < FlowState::kPasses; ++i) layouts[i] = (f.*kSetPass[i]).set_layout;
    for (uint32_t e = 0; e < g.n_cmd; ++e) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = f.pool; ai.descriptorSetCount = FlowState::kPasses; ai.pSetLayouts = layouts;
        if (dt.AllocateDescriptorSets(dt.device, &ai, f.sets[e]) != VK_SUCCESS) return false;
    }

    TimingInit(dt, g);
    f.ready = true;
    f.pair_bidir = Live().interp_bidir.load() != 0;
    FG_LOG("settings: phase_lock %d offset %.2f | interp_guide %.3f | conf %.3f..%.3f | flow_level %d subpixel %d | optical_flow %d | interp_cand %.3f | pan %.2f..%.2f | synth_margin %.2f | bwd_skip %d | bidir %d | ui_mask %.3f | extrap %d",
           int(GetConfig().phase_lock), double(GetConfig().phase_offset), double(GetConfig().interp_guide), double(GetConfig().conf_lo), double(GetConfig().conf_hi),
           GetConfig().flow_level, int(GetConfig().flow_subpixel), int(GetConfig().interpolate), double(GetConfig().cand_thr), double(GetConfig().pan_lo), double(GetConfig().pan_hi), double(GetConfig().synth_margin), int(SkipFinestBackward(f)), int(f.pair_bidir), double(GetConfig().ui_mask), int(GetConfig().extrapolate));
    FG_LOG("flow: optical-flow engine ready (%ux%u -> levels %ux%u, %ux%u, %ux%u, %ux%u; flow field at level %u: %ux%u; GPU timing %s)",
           g.extent.width, g.extent.height, ext[0].width, ext[0].height, ext[1].width, ext[1].height, ext[2].width, ext[2].height,
           ext[3].width, ext[3].height, f.fin + 1, ext[f.fin].width, ext[f.fin].height, f.qpool ? "on" : "unavailable");
    return true;
}

void FlowDestroy(const DeviceDispatch& dt, GpuState& g) {
    FlowState& f = g.flow;
    f.ready = false;
    if (f.qpool && dt.DestroyQueryPool) dt.DestroyQueryPool(dt.device, f.qpool, nullptr);
    f.qpool = VK_NULL_HANDLE;
    if (f.pool && dt.DestroyDescriptorPool) dt.DestroyDescriptorPool(dt.device, f.pool, nullptr);      // frees its sets
    f.pool = VK_NULL_HANDLE;
    for (auto& side : f.pyr) for (auto& i : side) DestroyFlowImage(dt, i);
    for (auto& i : f.fwd) DestroyFlowImage(dt, i);
    for (auto& i : f.bwd) DestroyFlowImage(dt, i);
    DestroyFlowImage(dt, f.mask);
    if (dt.DestroyShaderModule) for (auto* p : {&f.luma, &f.down, &f.coarse, &f.refine, &f.occlusion, &f.vis, &f.interp}) DestroyPass(dt, *p);
}

bool GpuFlow(const DeviceDispatch& dt, VkQueue queue, GpuState& g, const HeldFrame& a, const HeldFrame& b) {
    FlowState& f = g.flow;
    f.pair_valid = false;
    f.pair_bidir = Live().interp_bidir.load() != 0;
    if (!f.ready || f.failed) return false;
    std::lock_guard<std::mutex> lk(g.gpu_mu);
    uint32_t e;
    if (!BeginCmd(dt, g, e)) { f.failed = true; FG_LOG("flow: no command buffer, the engine is off"); return false; }
    VkCommandBuffer cb = g.cbs[e];

    // GPU time of the previous flow pass that used this entry (its fence has been waited for by BeginCmd)
    if (f.qpool && f.ts_pending[e]) {
        uint64_t t[FlowState::kQSlots];
        const uint32_t n = f.ts_n;
        if (n >= 2 && dt.GetQueryPoolResults(dt.device, f.qpool, FlowState::kQSlots * e, n, sizeof(uint64_t) * n, t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[n - 1] >= t[0]) {
            const double ms = double(t[n - 1] - t[0]) * double(f.ts_period_ns) * 1e-6;
            for (uint32_t i = 1; i < n; ++i) f.stage_sum[i - 1] += t[i] >= t[i - 1] ? double(t[i] - t[i - 1]) * double(f.ts_period_ns) * 1e-6 : 0.0;
            f.ms_sum += ms; f.ms_max = std::max(f.ms_max, ms);
            { f.stat_flow_ns += uint64_t(ms * 1e6); ++f.stat_flow_n; }
            if (++f.ms_count >= 120) {
                FG_LOG_ROUTINE("flow: GPU %.2f ms per pair (max %.2f) over %u pairs, field %ux%u", f.ms_sum / f.ms_count, f.ms_max, f.ms_count, f.fwd[f.fin].extent.width, f.fwd[f.fin].extent.height);
                char line[256]; int o = 0;
                for (uint32_t i = 0; i + 1 < n && o < 220; ++i) {
                    const char* name = i == 0 ? "pyramid" : i == 1 ? "coarse" : i + 2 == n ? "occlusion" : "refine";
                    int lvl = 0; if (i >= 2 && i + 2 < n) lvl = int(FlowState::kLevels) - 1 - int(i - 2);   // refine stages run from the second-coarsest level down to the finished one
                    o += lvl ? std::snprintf(line + o, sizeof line - size_t(o), "%s L%d %.2f | ", name, lvl, f.stage_sum[i] / f.ms_count)
                             : std::snprintf(line + o, sizeof line - size_t(o), "%s %.2f | ", name, f.stage_sum[i] / f.ms_count);
                }
                FG_LOG_ROUTINE("flow stages (ms, mean): %s", line);
                for (double& v : f.stage_sum) v = 0.0;
                f.ms_sum = 0.0; f.ms_max = 0.0; f.ms_count = 0;
            }
        }
        f.ts_pending[e] = false;
    }

    // ---- descriptors ----
    VkDescriptorImageInfo di[FlowState::kPasses * 6];
    VkWriteDescriptorSet wr[FlowState::kPasses * 6];
    uint32_t nw = 0, nd = 0;
    auto bind = [&](uint32_t set, std::initializer_list<VkImageView> views) {
        uint32_t binding = 0;
        for (VkImageView v : views) {
            di[nd] = {VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL};
            wr[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wr[nw].dstSet = f.sets[e][set]; wr[nw].dstBinding = binding++; wr[nw].descriptorCount = 1;
            wr[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[nw].pImageInfo = &di[nd];
            ++nw; ++nd;
        }
    };
    constexpr uint32_t L = FlowState::kLevels;
    bind(0, {g.ring[a.slot].view, f.pyr[0][0].view});
    bind(1, {g.ring[b.slot].view, f.pyr[1][0].view});
    for (uint32_t k = 1; k < L; ++k) {
        bind(2 + (k - 1), {f.pyr[0][k - 1].view, f.pyr[0][k].view});
        bind(5 + (k - 1), {f.pyr[1][k - 1].view, f.pyr[1][k].view});
    }
    bind(8, {f.pyr[0][L - 1].view, f.pyr[1][L - 1].view, f.fwd[L - 1].view});
    bind(9, {f.pyr[1][L - 1].view, f.pyr[0][L - 1].view, f.bwd[L - 1].view});
    for (int lv = int(L) - 2; lv >= 0; --lv) {                       // refine into level lv+1 (index lv): 3, 2, 1
        bind(10 + (int(L) - 2 - lv), {f.pyr[0][lv].view, f.pyr[1][lv].view, f.fwd[lv + 1].view, f.fwd[lv].view});
        bind(13 + (int(L) - 2 - lv), {f.pyr[1][lv].view, f.pyr[0][lv].view, f.bwd[lv + 1].view, f.bwd[lv].view});
    }
    bind(16, {f.fwd[f.fin].view, f.bwd[SkipFinestBackward(f) ? f.fin + 1 : f.fin].view, f.mask.view});
    dt.UpdateDescriptorSets(dt.device, nw, wr, 0, nullptr);

    // ---- commands ----
    constexpr auto ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr auto RW = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    if (f.qpool) {
        dt.CmdResetQueryPool(cb, f.qpool, FlowState::kQSlots * e, FlowState::kFlowStamps);
        dt.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, f.qpool, FlowState::kQSlots * e);
    }
    // first use: all engine images move to GENERAL
    {
        std::vector<VkImageMemoryBarrier2> first;
        auto add = [&](FlowImage& im) {
            if (im.initialized) return;
            VkImageMemoryBarrier2 bm{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            bm.srcStageMask = ALL; bm.srcAccessMask = 0; bm.dstStageMask = CS; bm.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            bm.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bm.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            bm.srcQueueFamilyIndex = bm.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bm.image = im.image; bm.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            first.push_back(bm); im.initialized = true;
        };
        for (auto& side : f.pyr) for (auto& im : side) add(im);
        for (auto& im : f.fwd) add(im);
        for (auto& im : f.bwd) add(im);
        add(f.mask);
        if (!first.empty()) {
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = uint32_t(first.size()); dep.pImageMemoryBarriers = first.data();
            (dt.CmdPipelineBarrier2 ? dt.CmdPipelineBarrier2 : dt.CmdPipelineBarrier2KHR)(cb, &dep);
        }
    }
    // everything queued before (captures, the previous pair's reads of these images) is done before the first pass
    MemBarrier(dt, cb, ALL, RW, CS, RW);

    VkPipeline bound = VK_NULL_HANDLE;
    auto dispatch = [&](FlowState::Pass FlowState::* which, uint32_t set, VkExtent2D out, const void* push, uint32_t push_bytes) {
        const FlowState::Pass& p = f.*which;
        if (bound != p.pipeline) { dt.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline); bound = p.pipeline; }
        dt.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &f.sets[e][set], 0, nullptr);
        if (push_bytes) dt.CmdPushConstants(cb, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes, push);
        dt.CmdDispatch(cb, (out.width + 15) / 16, (out.height + 15) / 16, 1);
    };
    uint32_t nstamp = 1;
    auto stamp = [&] {
        if (!f.qpool) return;
        if (nstamp < FlowState::kFlowStamps) dt.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.qpool, FlowState::kQSlots * e + nstamp++);
        else if (f.stamps_dropped++ == 0) FG_LOG("flow: more timestamps than slots (%u): the stage times are incomplete (the flow itself is not affected)", FlowState::kFlowStamps);
    };
    auto step = [&] { MemBarrier(dt, cb, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT); };

    dispatch(&FlowState::luma, 0, f.pyr[0][0].extent, nullptr, 0);
    dispatch(&FlowState::luma, 1, f.pyr[1][0].extent, nullptr, 0);
    step();
    for (uint32_t k = 1; k < L; ++k) {
        dispatch(&FlowState::down, 2 + (k - 1), f.pyr[0][k].extent, nullptr, 0);
        dispatch(&FlowState::down, 5 + (k - 1), f.pyr[1][k].extent, nullptr, 0);
        step();
    }
    stamp();   // pyramid done
    const int coarse_patch = kPatch[L - 1];
    dispatch(&FlowState::coarse, 8, f.fwd[L - 1].extent, &coarse_patch, sizeof(int));
    dispatch(&FlowState::coarse, 9, f.bwd[L - 1].extent, &coarse_patch, sizeof(int));
    step();
    stamp();   // coarse done
    for (int lv = int(L) - 2; lv >= int(f.fin); --lv) {                // down to the finished level
        const int push[3] = {kPatch[lv], kRadius[lv], (lv == int(f.fin) && GetConfig().flow_subpixel) ? 1 : 0};   // fractional vectors at the finished level only
        dispatch(&FlowState::refine, 10 + (int(L) - 2 - lv), f.fwd[lv].extent, push, sizeof(push));
        if (!(lv == int(f.fin) && SkipFinestBackward(f))) dispatch(&FlowState::refine, 13 + (int(L) - 2 - lv), f.bwd[lv].extent, push, sizeof(push));
        step();
        stamp();   // this refine level done
    }
    const bool skip_bwd = SkipFinestBackward(f);
    const float occl_pc[2] = {kOcclThreshold + (skip_bwd ? 1.0f : 0.0f), skip_bwd ? 2.0f : 1.0f};   // threshold, size of the backward field's pixels in forward-field pixels
    dispatch(&FlowState::occlusion, 16, f.fwd[f.fin].extent, occl_pc, sizeof(occl_pc));
    MemBarrier(dt, cb, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ALL, RW);
    bool stamped = false;                                                  // the stamps were written: read back when the entry is reused, but only if the submit below succeeds
    if (f.qpool) {
        stamp();   // occlusion done
        f.ts_n = nstamp;
        if (nstamp != FlowState::StampCount(f.fin) && !f.stamp_count_logged) { f.stamp_count_logged = true; FG_LOG("flow: %u timestamps written, %u expected: the stage names in the log may be off", nstamp, FlowState::StampCount(f.fin)); }
        stamped = true;
    }
    // VKIFRAME_DUMP: the copies into the pool are recorded in this command buffer, which is submitted anyway: no extra submit, wait or CPU copy
    const bool dumping = GetConfig().dump_pair > 0;
    const uint64_t n = g.pairs_flowed + 1, first = uint64_t(GetConfig().dump_pair), window = uint64_t(g.dump.frames);
    const bool in_window = dumping && g.dump_pool.ready && n >= first && n < first + window;
    if (in_window) {
        const char* dir = GetConfig().dump_dir;
        if (n == first) {
            struct stat dst;
            if (::mkdir(dir, 0700) != 0 && errno != EEXIST) FG_LOG("dump: cannot create %s (%s); set VKIFRAME_DUMP_DIR to a folder the game can write", dir, std::strerror(errno));
            else if (::lstat(dir, &dst) != 0 || !S_ISDIR(dst.st_mode) || dst.st_uid != ::geteuid()) FG_LOG("dump: %s is not a folder of this user, nothing written", dir);
            else { (void)::chmod(dir, 0700); FG_LOG("dump: pair %llu, capturing %llu frames into memory, the files are written after the last one (private: these are screenshots)", (unsigned long long)n, (unsigned long long)window); }
            DumpRecord(dt, cb, g.ring[b.slot].image, g.extent, 8, &g.dump_pool.B(), "B.rgba16f");
            auto dump_flow = [&](const FlowImage& img, VkExtent2D ext, uint32_t bpt, DumpPool::Buf* buf, const char* name) {
                if (img.transfer_src) DumpRecord(dt, cb, img.image, ext, bpt, buf, name);
                else FG_LOG("dump: %s skipped, the image cannot be a copy source on this driver", name);        // the flow engine itself is untouched
            };
            dump_flow(f.fwd[f.fin], f.fwd[f.fin].extent, 8, &g.dump_pool.Fwd(), "flow_fwd.rgba16f");
            dump_flow(f.bwd[f.fin], f.bwd[f.fin].extent, 8, &g.dump_pool.Bwd(), "flow_bwd.rgba16f");
            dump_flow(f.mask, f.fwd[f.fin].extent, 4, &g.dump_pool.Mask(), "mask.r32f");
        }
        char name[64]; std::snprintf(name, sizeof name, "pair%d_A.rgba16f", int(n - first));
        DumpRecord(dt, cb, g.ring[a.slot].image, g.extent, 8, &g.dump_pool.A(int(n - first)), name);
    }
    if (!EndAndSubmit(dt, queue, g, e, {}, VK_NULL_HANDLE)) {
        if (in_window) g.dump_pool.ClearRecorded();                                        // the copies never ran; teardown logs the drop
        f.failed = true; FG_LOG("flow: submit failed, the engine is off"); return false;
    }
    f.pair_valid = true;
    if (stamped) f.ts_pending[e] = true;
    if (dumping) {
        ++g.pairs_flowed;                                                                  // VKIFRAME_DUMP counts the pairs that went through GpuFlow
        if (in_window) {
            if (n < first + 5) g.dump.synth_left += 2;                                     // the synthetic frames of the first five pairs
            g.dump.pairs_queued = int(n - first) + 1;
            const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (!g.dump.AddTime(int(n - first), now_ns)) FG_LOG("dump: times.txt line for pair %d lost (out of memory)", int(n - first));
        }
        DumpMaybeFlush(dt, g, n >= first + window);          // the files are written after the last pair (or when the synthetic frames are in), never inside the window; the next pair forces it if some synthetic frames did not come
    }
    return true;
}
