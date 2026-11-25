## What this is

A C library and command line that lays out, identifies and renames git repos from one name.

- **Status and refusal** (`naula/status.h`): `naula_status` and the size-prefixed `naula_refusal`, which carries a
  stable `code`, a `detail` and a `fix` sentence. `NAULA_NOT_FOUND` is the only answer that says a home is dead.
- **One allocator** (`naula/memory.h`): every allocation in shipped code goes through `naula_memory_allocate` and its
  family. `naula` carries no allocator itself: each final binary links exactly one of `naula_allocator` and the
  test-only `naula_allocator_counting`, and the configure refuses a plain `malloc` anywhere else in `src/`.
