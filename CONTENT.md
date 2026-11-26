## What this is

A C library and command line that lays out, identifies and renames git repos from one name.

- **Status and refusal** (`naula/status.h`): `naula_status` and the size-prefixed `naula_refusal`, which carries a
  stable `code`, a `detail` and a `fix` sentence. `NAULA_NOT_FOUND` is the only answer that says a home is dead.
- **One allocator** (`naula/memory.h`): every allocation in shipped code goes through `naula_memory_allocate` and its
  family. `naula` carries no allocator itself: each final binary links exactly one of `naula_allocator` and the
  test-only `naula_allocator_counting`, and the configure refuses a plain `malloc` anywhere else in `src/`.
- **The resolver** (`naula/resolve.h`, `naula/session.h`, `naula/history.h`): from a dependency's `{ id, at }` to the
  homes that speak for that id. Trust starts at the numbers `at` holds, never at a name, and the answer is every home
  linked both ways to the first of them that confirms. The cache confirms nothing: it guards the number each account
  was confirmed at, keeps a number that answered gone as gone for good, and answers offline. Only a definite answer
  (a 404 by number, a provider that is not installed) counts a home as gone, so a timeout or an unresolved host never
  lets a copy past the home that would disown it.
