#ifndef NAULA_PROVIDER_GITHUB_HOOKS_H
#define NAULA_PROVIDER_GITHUB_HOOKS_H

/**
 * Points the GitHub provider at a loopback fixture: root, such as http://127.0.0.1:8080, replaces the API address of
 * every host; NULL restores them.
 *
 * @implNote Defined only when NAULA_TEST_HOOKS is set, which only the naula_test_hooks library does; the shipped
 * library carries no such symbol and always builds its address from the host.
 */
void naula_github_use_api_root(const char *root);

#endif
