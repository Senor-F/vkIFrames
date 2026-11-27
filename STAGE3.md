# STAGE3 (index)

This document is split into parts under 150 lines, moved verbatim, in their original order. Open only the part you need.

- [01-overview-objectives.md](stage3/01-overview-objectives.md)
  - 1. Overview & Objectives
  - 2. Repository Changes
  - 3. Configuration
  - 4. Implementation Details
    - A. Device creation: required features
    - B. Swapchain creation: image count, usage, checks
    - C. Ring buffer (`GpuState` in `include/gpu_frames.hpp`, `src/gpu_frames.cpp`)
    - D. Barrier helper (Synchronization2)
- [02-output-clock-per-tick-t-and-the-bypass-g.md](stage3/02-output-clock-per-tick-t-and-the-bypass-g.md)
    - E. Output clock, per-tick $t$ and the bypass gate (`include/output_clock.hpp`)
    - F. The present hook (hold-one, output clock)
    - G. Placeholder synthetic-frame shader (`shaders/blend.comp`)
  - 5. Verification & Testing Procedure
    - Step 1: Build, including shaders
    - Step 2: Enable the layer and cap the game
    - Step 3: Read the layer's own counters
    - Step 4: Success criteria
- [03-build-order-and-verification-log.md](stage3/03-build-order-and-verification-log.md)
  - 6. Build order and verification log
  - Addendum: applications that use VK_KHR_present_id (Proton / DXVK / vkd3d-proton)
  - Addendum: swapchain present fences (Proton/DXVK freeze)
  - Addendum: DXVK waits for every present (vkWaitForPresentKHR)
