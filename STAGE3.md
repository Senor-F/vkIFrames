
## Addendum: swapchain present fences (Proton/DXVK freeze)

Cuphead (DXVK, GE-Proton) froze after its first frame with holding on. Gamescope's WSI layer forces VK_EXT/KHR_swapchain_maintenance1,
so the game chains a VkSwapchainPresentFenceInfoKHR to each present and may wait for it. A held frame's fence only signals
when the layer finally shows it, so a game that waits on it before submitting the next frame deadlocks. Now:
- the fence is carried with the held frame and re-attached when it is presented (carrier keeps A's fence; hidden synthetic images carry none);
- vkWaitForFences / vkGetFenceStatus on a fence of a held frame, and vkWaitForPresentKHR on its id, present the held frame first (`flow_mu` serializes this with the present hook);
- the first present logs the game's pNext sTypes ("present chain of the first present");
- a watchdog on the presenter thread logs "watchdog: no present for N s | held | in acquire | in present-wait" when the game stops presenting.

Result of the first run of the fence build on the Deck: Cuphead's present chain is fence (1000275001) + present mode (1000275003) + present id (1000294000).
The present-mode struct was classed as unsupported, so holding switched itself off. It is now carried like the fence (re-attached to the carrier; hidden synthetic images carry none).

## Addendum: DXVK waits for every present (vkWaitForPresentKHR)

Run with the fence+mode build on the Deck: no freeze, but `synthetic 0`. DXVK calls vkWaitForPresentKHR for the id it just presented, and the first
fix answered that by showing the held frame at once, so nothing was ever held long enough to pair. Now a wait for the held frame's id is
answered immediately (the frame is only shown one frame late; waits for older ids are real). A game that then stops presenting gets its held
frame from an idle flush in the presenter thread (no present for 200 ms). Waits on fences of held frames still flush (the fence must really signal).
