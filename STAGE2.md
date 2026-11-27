# STAGE2 (index)

This document is split into parts under 150 lines, moved verbatim, in their original order. Open only the part you need.

- [01-overview-objectives.md](stage2/01-overview-objectives.md)
  - 1. Overview & Objectives
  - 2. Repository Changes
  - 3. Timing Engine
    - A. EMA Frame-Time Filter (`include/timing_engine.hpp`)
  - 4. Device and Swapchain Changes (`src/layer_main.cpp`)
- [02-device-creation-enable-what-the-driver-s.md](stage2/02-device-creation-enable-what-the-driver-s.md)
    - A. Device creation: enable what the driver supports
    - B. Swapchain creation
    - C. Choosing a time domain
- [03-presenting-with-timing-information.md](stage2/03-presenting-with-timing-information.md)
    - D. Presenting with timing information
  - 5. Verification & Testing Procedure
    - Quick path
    - Step 1: Build
    - Step 2: Enable the layer
    - Step 3: Run an app at a known base rate
    - Step 4: Success criteria
