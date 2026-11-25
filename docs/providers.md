# Providers

What each built-in provider reads, and what it was measured to answer. A capability a host does not offer is absent
from `describe` and answers REFUSED `unsupported_operation`; nothing fails silently.

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
| rename | `PATCH repos/{owner}/{repo}` with `name` | in the API definition; not exercised, nothing is written yet |
| transfer | `POST repos/{owner}/{repo}/transfer` with `new_owner` | in the API definition; not exercised |
| archive | `PATCH repos/{owner}/{repo}` with `archived` | in the API definition; not exercised |
| `release_index` | | unsupported |

Every route except the number lookup is by owner and name, so each read looks the repository up by number first, builds
the route from the names it answers, and looks it up by number again afterwards. A repository renamed in between is
refused `repository_changed`, so bytes are never taken from another repository that holds the name. A 404 on a file,
commit or ref is refused (`missing_file_or_ref`, `missing_commit`, `missing_commit_or_ref`) while the repository is live
and answers NOT_FOUND only once the lookup by number says it is gone; it never becomes "not contained".

### The minimum host: Gitea 1.20

`read_file`, `contains` and `parent_count` read `GET version` first and refuse a host older than Gitea 1.20 as
`unsupported_operation`, with a fix naming the minimum. Forgejo from 7 on reports its own version with the Gitea it
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
