# STAGE1 (index)

This document is split into parts under 150 lines, moved verbatim, in their original order. Open only the part you need.

- [01-overview-objectives.md](stage1/01-overview-objectives.md)
  - 1. Overview & Objectives
  - 2. Repository Layout (Stage 1 additions)
  - 3. Configuration & Metadata
    - A. Layer Manifest Template (`manifest/VkLayer_frame_gen.json.in`)
    - B. Build System (`CMakeLists.txt`)
  - 4. Implementation Details
- [02-dispatch-tables-shared-declarations.md](stage1/02-dispatch-tables-shared-declarations.md)
    - A. Dispatch Tables & Shared Declarations (`include/vk_iframes_layer.hpp`)
- [03-registries-proc-addr-tables-negotiation.md](stage1/03-registries-proc-addr-tables-negotiation.md)
    - B. Registries, Proc-Addr Tables, Negotiation (`src/layer_dispatch.cpp`)
- [04-hooks.md](stage1/04-hooks.md)
    - C. Hooks (`src/layer_main.cpp`)
- [05-verification-testing-procedure.md](stage1/05-verification-testing-procedure.md)
  - 5. Verification & Testing Procedure
    - Step 1: Compilation
    - Step 2: Enable the layer
    - Step 3: Run a Vulkan application
    - Step 4: Success criteria
