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
  `siblings` checks every dependency out at its commit; `new`, `adopt`, `add` and `retire` are the writing half,
  below. It reads history with the `git` it runs under (2.24 or newer,
  refused below with the fix), fetching each home's commits once a run into `<cache>/history`, and takes a host's token
  from `git credential fill`. Every git it runs has the variables that would point it at another repository,
  configuration or program (`GIT_DIR`, `GIT_CONFIG_*`, `GIT_EXEC_PATH`, `GIT_SSH` and the like) removed, so a hook or a
  CI step cannot point naula's git elsewhere. `GIT_CONFIG_GLOBAL` is among them: naula's git reads the user's own
  global configuration from where git looks by default (`~/.gitconfig`, or `git/config` under `XDG_CONFIG_HOME`), never
  from a file that variable names. One home's fetch is stopped and refused past 10 minutes (`fetch-too-slow`) or past
  1 GiB of history (`fetch-too-large`), however slowly the home trickles; `NAULA_FETCH_TIME_LIMIT_MS` and
  `NAULA_FETCH_BYTE_LIMIT` set other limits. A fetch stopped that way leaves none of git's temporary pack files
  (`tmp_pack_*`, `tmp_idx_*`) it wrote in `<cache>/history`; naula removes those it made and no other. Exit 0 done,
  1 refused (code, detail and fix on standard error), 2 usage; `resolve` and `check` take `--json`. An acceptance the
  resolver asks for is never remembered: each run names it and `--accept-primary` or `--accept-number` grants it for
  that run.
- **What a refused layout command leaves:** `init`, `hold`, `release`, `sync` and `clone` read every home first, and
  a repo of a held account or kept in `extra` that gets no definite answer (unreachable, refused, without its branch,
  with an invalid `repo.json`, unconfirmed) refuses the whole run as `repos-unread`, naming each, before any link,
  clone or file is touched, and so does a refused plan. A repo a home it lists disowns, or a fork
  of the repo holding its id, is a definite answer and skipped with a warning (`copy-skipped`), unless naula recorded a
  link for it, one carrying its name to a checkout of its id: then it refuses (`copy-linked`), since it may be our own
  repo with a mistake in its homes, whatever other repo holds the id. So a home that forges `disowned-by` against our
  own repo refuses the whole sync until our repo's homes are fixed: it fails closed, by design. An id two repos of the
  run carry, laid out or skipped as copies, refuses the whole run (`id-contested`, naming both and the link) when naula
  recorded a link to that id's checkout, whatever its name, so a fork of our id never takes our checkout or link; with
  no such link the two are left where they are (`id-collision`), and the plan never removes a recorded link to a
  checkout two repos' ids name. An
  empty repo with no branch is unadopted, a note, unless naula linked a repo of its name to a checkout no repo of the
  run holds: then it refuses (`empty-linked`) rather than remove that link. From the first write on, the run holds the
  root's lock: it writes
  `.naula.json`, clones what is missing straight into its `repos/<12 hex>` folder, then applies the plan journalled. A
  refusal after that point can leave `.naula.json` written and clones already in `repos/`, which the next run finds and
  keeps, and a journal the next run resumes; it never leaves a half-made link or move. `release` holds the lock for the
  whole command, and also removes the released org's `CLAUDE.md` while its bytes are exactly what naula's plan writes
  there, read and removed through one walk from the root; one someone edited is left and named.
- **`siblings`** resolves every dependency and checks that a branch of its primary holds its commit before the first
  clone, so a refused dependency clones nothing; a clone that then fails (a disk error, a folder git will not clone
  into) leaves the siblings already cloned. A link or reparse point anywhere in `siblings/<path>` below the repo, the
  `siblings` folder included, is refused, asked again right before each clone. Two paths that differ only in case are
  one path.
- **Reconciliation** (`naula/reconcile.h`, in `naula_git`): `naula sync` first brings the hosts of every adopted
  checkout under the root (`naula sync <org>`: of that org's folder alone) to the `repo.json` at the tip of the working branch of the home that serves it, never to
  the local file: rename the primary, then every other home; transfer a home whose account changed; create a missing
  home and push every branch and tag to it; push to every other home, never forcing; write the metadata the file
  states. Each step compares first, so a rerun after a host refused one converges and changes nothing once the hosts
  match. A home is written to only when its host shows it holds the id and, unless it serves the file, its own
  `repo.json` lists the serving home, so a copy can neither take writes nor steer them. A home is created, or a repo
  transferred into an account, only where an org of the root holds that account (its declaring repo's primary lives
  there) or the run names it with `--accept-home <provider>[@<host>]:<account>`, since whoever can push the file
  could otherwise have every branch pushed to an account of their choosing. Refused before anything is sent: that
  (`home-unaccepted`), a home that cannot be heard (`home-unheard`), a name another repo holds (`name-taken`), and a
  rename or transfer of a repo others reach by its name (`literal-consumers`: a `workflow_call` workflow, or a Gradle
  build with a release). Where each home was last found is kept as a hint in `<cache>/homes/<hex>-<anchor>.json`, the
  anchor hashing the homes the checkout's own `repo.json` history listed, so a fork's checkout never shares it; a hint
  is trusted only once the host confirms it again, and serves the file only for an account that history listed
  (`serving-unproven` otherwise). A host's refusal is reported and the sync answers `reconcile-incomplete` after laying
  out.
- **`new`, `adopt`, `add`, `retire`**: `new <org> <name> [--project p]` gives the repo the homes of its org's declaring
  repo, creates only in an account one of that repo's confirmed homes is under or one `--accept-home` names
  (`home-unaccepted`), refuses a name any of them holds before creating anything, makes the first commit (the `repo.json` alone, its
  hash the id) without a working tree, creates and fills every home, and clones and links it. `adopt` writes format 2
  in place and commits nothing: `name` from the primary host, `homes` from format 1 `hosts` or else from the remotes
  by account, each forge-number dependency as `{ id, at }` with the id derived from that repo's own history and its
  commit checked there, `claims` and `place` dropped, paths kept; format 1 `projects` are refused (`org-undeclared`)
  until the `org` member is written by hand, since adopt never invents an org; a file already in format 2 is only rewritten
  canonically, so a second run changes no byte. A first-parent root another repo of the primary's account shares is
  refused (`root-shared`) with the proposed `--fork-id`, since which repo takes its own identity is a person's call.
  `add` writes `{ id, at, importName, commit }`, `at` from every confirmed home and the commit from the primary's
  working branch. `retire` archives an old home only once it is confirmed in that run (a slow one is `home-unheard`),
  is not the primary, serves the file the primary serves (`listing-not-pushed`), and every dependent under the root
  reaches another home by its `at`.
- **`check`** refuses a shallow checkout (`fetch-depth: 0` in actions/checkout), takes a `--previous` of all zeros as
  no previous tip, and fails as `too-many-findings` rather than stop counting past 255 findings. A home that holds the
  name is this repo when the id confirms there, or, for a host repo with no format 2 `repo.json` yet, when its own
  history gives our id; one that cannot be heard is `name-unverifiable`, a warning.
