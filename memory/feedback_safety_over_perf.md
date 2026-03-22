---
name: Safety and readability over premature performance
description: Prefer safe, readable code over micro-optimisations that have no proven benefit yet
type: feedback
---

Prefer safety and readability over optimisations that are not yet justified by a profiler or a concrete bottleneck.

**Why:** User explicitly stated this as a guiding principle when choosing between QGraphicsObject (safer, QPointer-compatible) and QGraphicsItem (raw pointers, slightly less overhead).

**How to apply:** When there is a choice between a safer/clearer pattern and a faster but more fragile one, default to the safer one. Only diverge when there is a measured reason to do so.
