---
name: category-folder-not-prefix
description: "When moving or creating files, use a category folder and drop the shared prefix instead of keeping prefix_name files"
---

Files that share a category go in a folder named after the category, with the prefix dropped. Examples:
- `frontend.cpp`, `frontend_settings.cpp`, `frontend_settings.hpp` → `frontend/frontend.cpp`, `frontend/settings.cpp`, `frontend/settings.hpp`
- `sprite_units.cpp` → `sprites/units.cpp`
- `compile_roms.py` → `compile/roms.py`

When you create or move files:
- Name the destination folder-first and prefix-free, e.g. `runtime/debug/profile.cpp`, not `runtime/debug/block_profile.cpp`. Do the same for every sibling in the group, e.g. `discovery_log.*`.
- `git mv` the whole group and update every `#include`, CMake source list, import and doc reference.
- Do not leave forwarding headers or aliases at the old paths.
- Check the resulting file names against this rule before you report the move as done.

Not a violation: an underscore that is part of one concept rather than a shared category prefix, e.g. `main_loop.cpp` with no other `main_*` siblings.
