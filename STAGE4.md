
## Implementation notes (engine written)

- `src/optical_flow.cpp`: `FlowCreate/FlowDestroy/GpuFlow`. One command buffer per pair on the present queue, 18 descriptor sets per command-buffer entry
  (layout documented at the top of the file). Called from `ExecutePair` once per pair, before the tick loop, when `VKIFRAME_FLOW=1` or `VKIFRAME_DEBUG_FLOW=1`.
- Algorithm = `include/flow_ref.hpp` (tested by `flow_reference`): levels 1..4, exhaustive ±4 at level 4, refine with patch/radius {1,2,3,3}, forward + backward, consistency mask.
- `VKIFRAME_DEBUG_FLOW=1` replaces every synthetic frame with the motion-field picture (`flow_vis.comp`): hue = direction, saturation = length, white wash = occluded.
- GPU time: timestamp queries, logged every 120 pairs as `flow: GPU x.xx ms per pair`.
- The flow does not change the synthetic frames unless the debug view is on (interpolation is Stage 5).
- Shaders are NOT compiled in the sandbox: the first GPU build may report glslang errors. The mock tests skip the engine (stub shaders), so only the 9070 run exercises it.
