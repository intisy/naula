# Providers

What each built-in provider reads, and what it was measured to answer. A capability a host does not offer is absent
from `describe` and answers REFUSED `unsupported_operation`; nothing fails silently.

## Writes, on every provider

`create`, `rename`, `transfer`, `archive` and `write_metadata` are appended to the provider interface and read only
when a provider's size covers them (`naula_provider_offers_writes`); the registry keeps a plugin's own size, so a
plugin of the first shape offers none. GitHub, Gitea and Forgejo offer all of them and `read_metadata`; `web` offers
none. Nothing here has been sent to a real host: every write is tested against the loopback fixture only, and the
live proof (plan task 20) is the first real write.

- **Before any request**, a write refuses a missing token (`token_needed`) and a name or account that is not a
  plain name of at most 100 characters or that starts with `-` (`invalid_request`).
- **A write is never redirected.** The transport follows redirects for GET only; any other method answers its 3xx as
  a status, refused `redirected`, so a body and its token never reach an address the home does not name.
- **A 404 on a write** is settled by looking the number up: gone is NOT_FOUND, still there is `not_permitted` (the
  token lacks admin rights), and on Gitea and Forgejo a repository renamed between the lookup and the write is
  `repository_changed`.
- **A name the host refuses**: 409 on `create` or `rename` is `name_taken`; 422 is `refused_by_validation`, since
  the transport keeps no body of a refused answer and a 422 can mean a taken name, a reserved one (Gitea) or another
  validation failure.
- **`create`** first asks whether the account is an org (`GET orgs/{account}`); if not, it creates under the token's
  user only when `GET user` answers that same login, and is refused `account_not_writable` otherwise, so a
  repository is never created under the token's user in place of the account named. It always sends `private`:
  true unless the metadata's `settings.visibility` is `public`, so a repository is never public before its file says
  so, and a visibility that is neither is refused `invalid_request` before any request, as `write_metadata` refuses
  it. The caller writes the rest of the metadata with `write_metadata`. An answer under another account or name is
  refused `created_elsewhere`.
- **`write_metadata`** sends only the members whose bit is in `present`, and answers in `applied` what it wrote,
  also when it fails part way. A member counts as written only when the repository the PATCH answers shows it as
  sent, so a member a host takes without error and ignores is not reported applied; GitHub's topics are compared with
  the list its PUT answers, and Gitea's are taken on its 204, which carries none. `settings` maps `visibility` to
  `private`, `defaultBranch` to `default_branch`, `issues` to `has_issues` and `wiki` to `has_wiki`; a setting the
  host lacks is left out, and `settings` counts as written only when one of its settings was sent and every sent one
  shows. A host without topics leaves `topics` out of `applied`.

| Code | Meaning |
|---|---|
| `token_needed` | a write was asked without a token |
| `invalid_request` | an account, name, metadata or visibility naula will not send |
| `redirected` | the host answered a write with a redirect, which is never followed |
| `not_permitted` | a 404 on a write while the repository is live: the token lacks the rights |
| `repository_changed` | Gitea and Forgejo: the names changed between the lookup and the write (a 404, or the 307 Gitea answers for an old name); run it again once the rename has settled |
| `wrote_another_repository` | Gitea and Forgejo: the write's answer carries another number, so another repository took the name in between and was written; the detail names both numbers and the name to repair |
| `name_taken` | 409 on `create` or `rename` |
| `refused_by_validation` | 422 on `create` or `rename`: the name may be taken or reserved |
| `rename_not_applied` | the rename's answer does not carry the new name |
| `archive_not_applied` | the archive's answer does not say archived |
| `transfer_refused` | 422 on `transfer`: the new owner is missing, refuses, or holds that name |
| `account_not_writable` | `create` under an account that is neither an org nor the token's user |
| `created_elsewhere` | `create` answered with another account or name |
| `number_mismatch` | GitHub answered a write by number with another repository |
- **An account rename** is refused `unsupported_operation` by `naula_refuse_account_rename` with the fix to rename it
  on the website and then change `account` in `homes` (spec 7.4, whose premise that not every provider's API offers
  it is unmeasured); naula renames no account, and `describe` has no bit for it.

## GitHub

`contains` reads `GET repositories/{id}/compare/{commit}...{ref}?per_page=1` and answers contained when `status` is
`ahead` or `identical`. A 404 on the compare reads the same as a repository that is gone, so the repository is looked up
by number: while it is live the 404 is refused `missing_commit_or_ref`, and only a 404 on that lookup answers NOT_FOUND.
It is never read as "not contained", which the identity check would act on. A ref starting with `-` is refused before
any request, as Gitea and Forgejo refuse it, so both providers refuse the same inputs.

Every write but `create` goes to the repository's number, the route GitHub itself redirects a renamed repository to,
so a write never reaches a repository that took the old name. That route is undocumented (spec section 9, known
limits) and measured for reads only; whether GitHub takes a write on it is first shown by the live proof.

| Operation | Endpoint |
|---|---|
| `create` | `POST orgs/{account}/repos` or `POST user/repos` with `name` and `private`; the probe measured on 2026-11-26: `GET orgs/github` 200, `GET orgs/octocat` (a user) 404 |
| `rename` | `PATCH repositories/{id}` with `name`; the answer must carry the new name |
| `transfer` | `POST repositories/{id}/transfer` with `new_owner`; 422 is `transfer_refused` |
| `archive` | `PATCH repositories/{id}` with `archived: true`; the answer must say archived |
| `read_metadata` | `GET repositories/{id}`: `description`, `homepage`, `topics`, `private`, `default_branch`, `has_issues`, `has_wiki`, every one present in the anonymous answer for 1296269 on 2026-11-26 |
| `write_metadata` | `PATCH repositories/{id}` with the members above, then `PUT repositories/{id}/topics` with `names` |

## Gitea and Forgejo

One module, `src/provider_gitea.c`, answers both words from one route table. Measured on 2026-11-25 against
codeberg.org (Forgejo 17.0.0-121, API of Gitea 1.22.0) by reading its `swagger.v1.json` and anonymous GETs of the
public repo `Codeberg/pages-server`. Nothing was created or changed. The API sends no version header, so none is sent;
only `Accept: application/json`.

| Operation | Endpoint | Measured |
|---|---|---|
| `lookup(number)` | `GET repositories/{id}` | 401 for an anonymous caller, even for a number that does not exist, so anonymous lookup is off in `describe` and a gone repo reads as "token needed" until a token is given |
| `find(account, name)` | `GET repos/{owner}/{repo}` | anonymous, 200 or 404; the answered owner and name are compared with the request, case-insensitively |
| `read_file` | `GET repos/{owner}/{repo}/raw/{path}?ref=` | anonymous, raw bytes, 404 for a missing file or ref |
| `contains` | `GET repos/{owner}/{repo}/commits?sha={commit}&not={ref}&limit=1&stat=false&verification=false&files=false` | the commits `commit` holds that `ref` lacks; see below |
| `parent_count` | `GET repos/{owner}/{repo}/git/commits/{sha}?stat=false&verification=false&files=false` | `parents` array |
| version | `GET version` | `{"version":"17.0.0-121-39320c0+gitea-1.22.0"}` on Codeberg |
| fork parent | `parent.id` in the repository answer | present, null for a repo that is no fork |
| `list_repos` | `GET orgs/{org}/repos`, then `users/{user}/repos`, `limit` and `page` | `limit` is clamped to the host's `max_response_items` (50 on Codeberg), so pages are read until an empty one |
| `create` | `GET orgs/{account}`, else `GET user`; then `POST orgs/{account}/repos` or `POST user/repos` with `name` and `private` | the probe measured on 2026-11-26: `GET orgs/Codeberg` 200, a name that is no org 404; the `POST` not exercised, 409 is a taken name |
| `rename` | `PATCH repos/{owner}/{repo}` with `name` | in the API definition; not exercised, 422 is a taken (or reserved) name |
| `transfer` | `POST repos/{owner}/{repo}/transfer` with `new_owner` | in the API definition; not exercised, 202 with the repository |
| `archive` | `PATCH repos/{owner}/{repo}` with `archived` | in the API definition; not exercised |
| `read_metadata` | `GET repos/{owner}/{repo}` (`description`, `website`, `private`, `default_branch`, `has_issues`, `has_wiki`) and `GET repos/{owner}/{repo}/topics` | anonymous GETs on Codeberg on 2026-11-26 answer every member, and the topics as `{"topics": [...]}` |
| `write_metadata` | `PATCH repos/{owner}/{repo}` with those members, then `PUT repos/{owner}/{repo}/topics` with `topics` | in the API definition; not exercised, `PUT` answers 204 |
| `release_index` | | unsupported |

Gitea and Forgejo have no write route by number, so a write by name can land on another repository, and naula can
only keep that window short and say when it happened. A write looks the number up and reads the version, then looks
the number up again immediately before each request that writes (so `write_metadata`'s PATCH and PUT each have their
own), and writes to the names that last lookup answered. The window left is the time between that lookup and the
write. If inside it this repository is renamed and another repository is created under its old name, the write lands
on that other repository: its answer carries another number, and the write is refused `wrote_another_repository` with
both numbers and the name, so the person can check and repair it. A plain rename inside the window answers 307
(Gitea's redirect of an old name), which is never followed: the number is looked up again and the write is refused
`repository_changed`. `read_metadata` reads by the looked-up names and looks the number up again afterwards, as every
read does.

Every route except the number lookup is by owner and name, so each read looks the repository up by number first, builds
the route from the names it answers, and looks it up by number again afterwards. A repository renamed in between is
refused `repository_changed`, so bytes are never taken from another repository that holds the name. A 404 on a file,
commit or ref is refused (`missing_file_or_ref`, `missing_commit`, `missing_commit_or_ref`) while the repository is live
and answers NOT_FOUND only once the lookup by number says it is gone; it never becomes "not contained".

### The minimum host: Gitea 1.20

`read_file`, `contains`, `parent_count` and every write read `GET version`, after the lookup by number, and refuse a
host older than Gitea 1.20 as `unsupported_operation`, with a fix naming the minimum. The lookup comes first so a
host that wants a token, or has lost the repository, answers that rather than its age. Forgejo from 7 on reports its own version with the Gitea it
carries after `+gitea-` (`7.0.0+gitea-1.22.0`), which is the part read; Gitea, and Forgejo before 7 (`1.21.11-1`), lead
with the Gitea version. A host that answers 404 for its version is refused the same way, never NOT_FOUND. What sets
the minimum, read from go-gitea/gitea on 2026-11-26:

| Relied on | First release | Source |
|---|---|---|
| `not` on `GET repos/{owner}/{repo}/commits` | 1.20.0 | #24409 "Add ability to specify '--not' from GetAllCommits", `routers/api/v1/repo/commits.go` at `v1.20.0` reads `not` and passes it to `CommitsByRange`; absent at `v1.19.0` |
| `stat`, `verification`, `files` on that list | 1.20.0 (`stat` since 1.18.0) | #24568 "Filters for GetAllCommits", #21337 for `stat` |
| `stat`, `verification`, `files` on `git/commits/{sha}` | 1.20.0 | #24613 "Filter get single commit" |
| `ref` on `GET repos/{owner}/{repo}/raw/{path}` | 1.14.0 | #14602 "Add support for ref parameter to get raw file API" |
| `GET repos/{owner}/{repo}/git/commits/{sha}` | before 1.10.0 | `routers/api/v1/api.go` at `v1.10.0` |
| `PATCH repos/{owner}/{repo}` with `name`, `description`, `website`, `private`, `has_issues`, `has_wiki`, `default_branch`, `archived` | 1.9.0 | #7006 "Adds API Endpoint for Repo Edit"; `EditRepoOption` in `modules/structs/repo.go` at `v1.9.0` carries every one, absent at `v1.8.0` |
| `GET` and `PUT repos/{owner}/{repo}/topics` | 1.10.0 | #7963 "Add API endpoint for accessing repo topics"; the `/topics` group in `api.go` at `v1.10.0`, absent at `v1.9.0` |
| `POST repos/{owner}/{repo}/transfer` with `new_owner` | 1.12.0 | #9947 "Add API endpoint for repo transfer"; in `api.go` at `v1.12.0`, absent at `v1.11.0` |
| `POST orgs/{org}/repos`, `POST user/repos`, `GET orgs/{org}`, `GET user` | before 1.8.0 | in `api.go` at `v1.8.0` |

**No write raises the minimum**: the newest route a write needs arrived in 1.12.0 (read from go-gitea/gitea on
2026-11-26 with `gh api`), so 1.20 stays the one floor for the provider, and a write checks it as a read does.

A host that ignores `not` answers the commit's whole history, which would read as "not contained"; a host that ignores
the three filters answers more than is read. Both are why the version is checked before either route is asked.

### How `contains` is decided

`commits?sha={commit}&not={ref}` lists the commits `commit` holds that `ref` lacks, the `git rev-list commit --not ref`
of the host. An empty list means the branch contains the commit. Measured on `Codeberg/pages-server` with `limit=1`:

| `sha` | `not` | Answer |
|---|---|---|
| an ancestor of `main` | `main` | 200 `[]` |
| the tip of `main` | `main` | 200 `[]` |
| `main` | an ancestor of `main` | 200, one commit |
| a commit on a diverged branch (`feat/redis-caching`) | `main` | 200, one commit |
| the tip of the unrelated `pages` branch, which has its own root | `main` | 200, one commit |
| a commit the repo does not hold | `main` | 404 `null`, refused `missing_commit_or_ref` |
| an ancestor of `main` | a branch that does not exist | 500, refused `unexpected_status` |
| `feat/redis-caching`'s tip | `feat%2Fredis-caching` | 200 `[]`, so a ref with a slash is sent encoded |

Unrelated histories need no tip check: no commit of the other history is reachable from the branch, so the list is
never empty. A ref starting with `-` is refused before any request, so it cannot read as an option to the host's git.
The answer is one commit at most, about 3 KB whatever the length of the history.

### Known limits

- A token is needed for every call that starts from a number, which is all of them but `find` and `list_repos`.
  `find` reaches a number without one, and the number is then asked for with a token.
- The version is read on every call that relies on it, not kept, since a provider is a constant shared by every caller.
- The tests record real Forgejo answers under `test/fixtures/forgejo` (the commit answer without `files` and `stats`;
  `commits_contained.json`, `commits_diverged.json` and `commits_unrelated.json` as the host sent them). The one live
  test, `provider_network_test`, carries the `network` label.

## Web

`naula_web_document(domain, hex)` reads `https://<domain>/.well-known/repo-identity/<hex>.json` and returns its bytes
when the document is an object with an `id` naming that same `git:<hex>` and a `homes` list. The `web` provider
supports no `NAULA_OP_` operation, since a domain has no number to look up.

- Only a 404 answers NOT_FOUND (dead, spec section 7.3). A domain that does not resolve answers UNREACHABLE, refused
  `host_not_resolved` with the network fix, and a timeout answers TIMED_OUT: neither says the domain is gone. Neither
  libcurl's `CURLE_COULDNT_RESOLVE_HOST` nor WinHTTP's 12007 tells an authoritative NXDOMAIN from an offline machine
  or a blocked resolver, and reading it as dead would let whoever answers one DNS query silence a home that disowns a
  copy. The transport still marks the case in `naula_fetch_answer.host_unresolved`, which only picks the refusal code.
- A document for another id is refused `document_for_another_id`; one that is not an identity document is MALFORMED.

## Nothing local orders a re-registration

When an account is deleted and registered again, the repository made there has a new number. A consumer with its
own record refuses that number as `number-changed`. Two of an id's records can also disagree on an account's number,
when one consumer's first contact took the new repository. Nothing on the machine says which number is the genuine
one:

- Numbers do not order it. GitHub does not document its repository ids as sequential, and a repository transferred
  into the re-registered account keeps its older, lower number; Gitea and Forgejo number a repository when it is made,
  which a transfer keeps too.
- Confirmation times do not order it. A record written while the clock was behind carries early times, and a clock
  behind now cannot tell a future time from a forged one.

So a lineage that disagrees on an account's number is never settled by naula: the session's `accept_number` is asked,
naming each number the lineage holds and the one found, and without an answer the home that lists the account stays
unverifiable. A `web` home has no number and is never asked about.
