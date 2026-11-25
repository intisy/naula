#ifndef NAULA_PROVIDER_GITEA_HOOKS_H
#define NAULA_PROVIDER_GITEA_HOOKS_H

/**
 * Points the Gitea and Forgejo providers at a loopback fixture: root, such as http://127.0.0.1:8080/api/v1, replaces the
 * API address of every host; NULL restores them. A call that names no host is still refused.
 *
 * @implNote Defined only when NAULA_TEST_HOOKS is set, which only the naula_test_hooks library does; the shipped
 * library carries no such symbol and always builds its address from the host.
 */
void naula_gitea_use_api_root(const char *root);

#endif
