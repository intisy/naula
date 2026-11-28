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
- **The command line** (`naula`, `src/cli`): `init`, `hold`, `release`, `sync` and `clone` lay a root out from the
  orgs it holds; `resolve` prints the homes that speak for an id; `check` is the gate a repo passes before a push;
  `siblings` checks every dependency out at its commit. It reads history with the `git` it runs under (2.24 or newer,
  refused below with the fix), fetching each home's commits once a run into `<cache>/history`, and takes a host's token
  from `git credential fill`. Every git it runs has the variables that would point it at another repository,
  configuration or program (`GIT_DIR`, `GIT_CONFIG_*`, `GIT_EXEC_PATH`, `GIT_SSH` and the like) removed. One home's
  fetch is stopped and refused past 10 minutes (`fetch-too-slow`) or past 1 GiB of history (`fetch-too-large`), however
  slowly the home trickles; `NAULA_FETCH_TIME_LIMIT_MS` and `NAULA_FETCH_BYTE_LIMIT` set other limits. Exit 0 done,
  1 refused (code, detail and fix on standard error), 2 usage; `resolve` and `check` take `--json`. An acceptance the
  resolver asks for is never remembered: each run names it and `--accept-primary` or `--accept-number` grants it for
  that run.
- **What a refused layout command leaves:** `init`, `hold`, `release`, `sync` and `clone` read every home first, and
  a repo of a held account or kept in `extra` that gets no definite answer (unreachable, refused, without its branch,
  with an invalid `repo.json`, unconfirmed) refuses the whole run as `repos-unread`, naming each, before any link,
  clone or file is touched, and so does a refused plan. A repo a home it lists disowns, or a fork
  of the repo holding its id, is a definite answer and skipped with a warning, unless naula linked a checkout of
  that id and no repo laid out holds it, which may be our own repo with a mistake in its homes; an empty repo with no
  branch is unadopted, a note. From the first write on, the run holds the root's lock: it writes
  `.naula.json`, clones what is missing straight into its `repos/<12 hex>` folder, then applies the plan journalled. A
  refusal after that point can leave `.naula.json` written and clones already in `repos/`, which the next run finds and
  keeps, and a journal the next run resumes; it never leaves a half-made link or move. `release` also removes the
  released org's `CLAUDE.md` when it is still the one-line import naula wrote, and names one someone edited.
- **`siblings`** resolves every dependency and checks that a branch of its primary holds its commit before the first
  clone, so a refused dependency clones nothing; a clone that then fails (a disk error, a folder git will not clone
  into) leaves the siblings already cloned. A link or reparse point anywhere in `siblings/<path>` below the repo, the
  `siblings` folder included, is refused, asked again right before each clone. Two paths that differ only in case are
  one path.
- **`check`** refuses a shallow checkout (`fetch-depth: 0` in actions/checkout), takes a `--previous` of all zeros as
  no previous tip, and fails as `too-many-findings` rather than stop counting past 255 findings. A home that holds the
  name is this repo when the id confirms there, or, for a host repo with no format 2 `repo.json` yet, when its own
  history gives our id; one that cannot be heard is `name-unverifiable`, a warning.
