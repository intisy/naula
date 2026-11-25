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
| `contains` | `GET repos/{owner}/{repo}/compare/{base}...{head}` | answers `total_commits` and `commits`, no status and no merge base; see below |
| `parent_count` | `GET repos/{owner}/{repo}/git/commits/{sha}` | `parents` array; also accepts a branch name |
| fork parent | `parent.id` in the repository answer | present, null for a repo that is no fork |
| `list_repos` | `GET orgs/{org}/repos`, then `users/{user}/repos`, `limit` and `page` | `limit` is clamped to the host's `max_response_items` (50 on Codeberg), so pages are read until an empty one |
| rename | `PATCH repos/{owner}/{repo}` with `name` | in the API definition; not exercised, nothing is written yet |
| transfer | `POST repos/{owner}/{repo}/transfer` with `new_owner` | in the API definition; not exercised |
| archive | `PATCH repos/{owner}/{repo}` with `archived` | in the API definition; not exercised |
| `release_index` | | unsupported |

Every route except the number lookup is by owner and name, so each read looks the repository up by number first, builds
the route from the names it answers, and looks it up by number again afterwards. A repository renamed in between is
refused `repository_changed`, so bytes are never taken from another repository that holds the name.

### How `contains` is decided

`compare/{base}...{head}` lists the commits `head` has that `base` lacks. Measured on `Codeberg/pages-server`:

| Request | `total_commits` |
|---|---|
| `main...<ancestor of main>` | 0 |
| `<ancestor of main>...main` | 3 |
| `main...<commit on a diverged branch>` | 15 |
| `main...<commit on an unrelated branch>` | 0, and 0 the other way too |

So a commit is in the branch when `ref...commit` is 0 and either `commit...ref` is above 0 or the branch tip is the
commit. The last row is why a lone zero is not enough: two histories with no common base answer 0 both ways, as an
identical commit does, and only the tip check tells them apart. A commit or ref the host does not know answers 404, which
the provider turns into "not contained" while the repository is live and into NOT_FOUND once it is gone.

The compare answer carries the changed files too, so a very large divergence can exceed the 8 MiB read limit and is
then refused `answer_too_large` rather than guessed.

### Known limits

- A token is needed for every call that starts from a number, which is all of them but `find` and `list_repos`.
  `find` reaches a number without one, and the number is then asked for with a token.
- The tests record real Forgejo answers under `test/fixtures/forgejo` (the commit answer without `files` and `stats`,
  the compare answers without `files` and with only the `sha` of each commit). The one live test,
  `provider_network_test`, carries the `network` label.

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
