## What this is

A C library and command line that lays out, identifies and renames git repos from one name.

- **Status and refusal** (`naula/status.h`): `naula_status` and the size-prefixed `naula_refusal`, which carries a
  stable `code`, a `detail` and a `fix` sentence. `NAULA_NOT_FOUND` is the only answer that says a home is dead.
- **One allocator** (`naula/memory.h`): every allocation in shipped code goes through `naula_memory_allocate` and its
  family. `naula` carries no allocator itself: each final binary links exactly one of `naula_allocator` and the
  test-only `naula_allocator_counting`, and the configure refuses a plain `malloc` anywhere else in `src/`.
- **Strict JSON** (`naula/json.h`): `naula_json_parse` reads bytes only as JSON itself does (well-formed UTF-8,
  JSON's grammar, no NUL, no deeper nesting than cJSON reads) and tells `NAULA_MALFORMED` from
  `NAULA_OUT_OF_MEMORY`, never one for the other. It keeps a duplicated key as often as it is written, so the repo
  file reader refuses one; `naula_json_parse_keeping_last_value` keeps its first position and last value, as
  JavaScript's `JSON.parse` does. `naula_json_is_valid_utf8` and `naula_json_is_grammar` are the two checks alone.
- **Ids and places** (`naula/id.h`, `naula/home.h`): `naula_id_parse` reads `git:<hex>` or `local:<name>` with
  path segments, and `naula_id_is_valid` answers the same question without allocating. `naula_place_parse` reads a
  place, and `naula_place_text` writes it in the normal form an `at` is kept in, hosts and domains lowercased and
  GitHub's own host left out.
- **Providers** (`naula/provider.h`): GitHub, Gitea and Forgejo are built in, and a plugin adds a word. Each reads a
  repository by its number, including `release_index`, the named asset of its latest or tagged release with the address
  that release offers it at. What each host was measured to answer is in `docs/providers.md`.
- **The resolver** (`naula/resolve.h`, `naula/session.h`, `naula/history.h`): from a dependency's `{ id, at }` to the
  homes that speak for that id. Trust starts at the numbers `at` holds, never at a name, and the answer is every home
  linked both ways to the first of them that confirms. The cache keeps one record per normalised `at`, named by its
  full sha256 and stating that `at`, and reads nothing another consumer wrote except, for an `at` with no record yet,
  its lineage: the records that confirmed a place both `at`s name. A record confirms nothing on its own: it refuses a
  number other than the one an account was confirmed at, starts the search only when every place of `at` answered
  gone, and answers offline. A run that confirms nothing never
  demotes a home. Records are never removed: nothing collects old ones yet. Only a definite answer in the run that
  heard it (a 404 by number, a provider that is not installed) counts a home as gone, so a timeout or an unresolved
  host never lets a copy past the home that would disown it. A home on a provider that is not installed is
  `dead-skipped`, and a refusal it decides names "install a plugin that provides a resolver" as its `fix`.
