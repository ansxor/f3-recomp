---
name: category-folder-not-filename-prefix
description: "Group category files in a folder (frontend/settings.cpp) instead of giving them a category filename prefix (frontend_settings.cpp)"
condition: "\\b(?:frontend|netplay|hle|sound|audio|gpu|video|renderer)_[A-Za-z0-9]+(?:_[A-Za-z0-9]+)*\\.(?:c|cc|cpp|h|hpp|mm)\\b"
scope: ["tool:vibe_send", "tool:vibe_spawn"]
---

When several files belong to one category, put them in a folder named after the category and remove the prefix from their names. For example, `frontend.cpp`, `frontend_settings.cpp` and `frontend_settings.hpp` become `frontend/frontend.cpp`, `frontend/settings.cpp` and `frontend/settings.hpp`.

- Never create, move or brief a worker toward a file named `<category>_<name>.*`. Use `<category>/<name>.*`.
- When you move prefixed files into a category folder, remove the prefix in the same move: `runtime/frontend_ui.cpp` → `runtime/frontend/ui.cpp`. Never leave `frontend/frontend_ui.cpp` and plan a second rename later.
- Only the folder's main entry file keeps the category name, e.g. `frontend/frontend.cpp`.
- Update every include, CMake source list and doc reference in the same change. Check for basename collisions across include dirs; where one exists, use a directory-qualified include such as `"frontend/settings.hpp"`.