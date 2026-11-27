# AUDIT (index)

This document is split into parts under 150 lines, moved verbatim, in their original order. Open only the part you need.
It is a running log: new entries go at the end of the latest part; a new numbered part starts when it would pass 150 lines, and is added here.

- [01-priority-order.md](audit/01-priority-order.md)
  - Priority order (owner, 22:24; reordered 06:00)
  - 1. Measurement logic for the Builder (first; so the Auditor can audit, compute and close)
  - 2. Refresh-rate clamp and cost warning (small; not implemented)
  - 3. Flow passes: tiling, then precision (the biggest lever)
  - 4. Ring and output format (16-bit float ring, 8-bit swapchain)
  - 5. Locking, command pools and descriptors
  - 6. Barriers that name every pipeline stage
  - 7. Timestamps and wakeups
  - 8. Fewer submits
  - 9. Async compute queue (last; only if the measurements justify it)
  - 10. Needed from the owner (Deck)
  - 11. Checked and closed (owner agrees, 22:24)
- [02-rules-for-these-notes.md](audit/02-rules-for-these-notes.md)
  - 12. Rules for these notes
  - 13. Review log, sanitizer results, measurements and residuals (2026-11-01, `main` c5230b8; updated for a831532, alpha.2 published)
