---
name: Project structure preference
description: User wants multi-library CMake layout with main as an integration root, not a monolithic single-target build
type: feedback
---

Prefer a proper multi-library CMake structure where `main.cpp` is a thin integration root that links several libraries. Even when a feature is not yet implemented (e.g., graph I/O), stub the library and link it in so the architecture reflects future plans from day one.

**Why:** User explicitly rejected a single flat CMakeLists.txt that put all sources in the same target as main.

**How to apply:** For any new C++ project in this repo, default to `add_subdirectory` layout with each logical subsystem as its own static library target, linked into the executable via `target_link_libraries`.
