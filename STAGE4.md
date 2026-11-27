# STAGE4 (index)

This document is split into parts under 150 lines, moved verbatim, in their original order. Open only the part you need.

- [01-overview-objectives.md](stage4/01-overview-objectives.md)
  - 1. Overview & Objectives
  - 2. Repository Changes
  - 3. Queue Selection and Device Creation: the plan for the async queue (not built)
    - A. Pick a compute family
    - B. Add the queue in `Hook_CreateDevice`
    - C. Learn which family the game's queue belongs to
- [02-shader-implementations.md](stage4/02-shader-implementations.md)
  - 4. Shader Implementations
    - A. Pyramid (`shaders/flow_luma.comp`, `shaders/flow_down.comp`)
    - B. Coarse optical flow (`shaders/flow_coarse.comp`)
    - C. Per-level refinement (`shaders/flow_refine.comp`)
    - D. Forward-backward occlusion check (`shaders/flow_occlusion.comp`)
  - 5. Optical Flow Engine (`src/optical_flow.cpp`)
  - 6. Build: Compile and Embed SPIR-V
  - 7. Verification & Testing Procedure
    - Step 1: Build and run the reference test
    - Step 2: Debug view and dump
    - Step 3: Success criteria
  - Implementation notes (engine written)
