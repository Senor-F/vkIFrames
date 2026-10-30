# Rules for every session working on this repository

## Roles
- The Builder is the only one who changes code. The Auditor reviews and keeps AUDIT.md. The PM keeps the plan and the .md docs.
- Nothing is merged until the owner has reviewed it and said so. The PM may do the merge after that. The Auditor never merges.
- Board: the issue titled "vkIFrames board" (#2). Start every comment with your role tag: [PM], [Builder] or [Auditor]. Do not post in vkIFrames-old.

## Commits, PRs, comments
- End commit messages with the Co-Authored-By line only. Never add a Claude-Session line.
- No link to a specific Claude Code session, chat or share anywhere: files, commits, PR descriptions, comments. The plain generic https://claude.ai/code footer is fine.
- After opening or editing a PR or comment, read it back and remove any link to a specific session.
- Do not create new Project docs.

## Anonymity
- Never write the owner's real name, email, IP address, hostnames or real home paths. Use placeholders such as /home/deck.
- Commit timestamps stay in UTC: set TZ=UTC for every session before committing.

## Design decisions
- The project stays a Vulkan layer. No DXVK fork. A proxy DLL front end is deferred.
- No web page and no listening network port.
- The core layer and the Decky plugin stay.
- In-game UI: the hotkeys F7 (open the menu) and F8 (layer on/off) open an in-game overlay. Users can change the keys. The overlay uses the same live settings file as the Decky plugin (see decky/LIVECONF.md), so the logic is shared. A command-line vkiframes-ctl also writes that file. This comes after the work in the order below.
- Order of work (owner decision, 2026-10-30): (1) measurement logic and bench.json; (2) refresh-rate clamp; (3) GPU and CPU load, after the owner has run bench.json on the Deck; (4) async queue last, only if bench.json shows it is worth it.
- Settings use the VKIFRAME_* names. The old FRAMEGEN_* names are still read as a fallback.
