#ifndef NAULA_PROVIDER_WEB_HOOKS_H
#define NAULA_PROVIDER_WEB_HOOKS_H

/**
 * Points the web provider at a loopback fixture: root, such as http://127.0.0.1:8080, replaces https://<domain> for
 * every domain; NULL restores them.
 *
 * @implNote Defined only when NAULA_TEST_HOOKS is set, which only the naula_test_hooks library does; the shipped
 * library carries no such symbol and always builds its address from the domain.
 */
void naula_web_use_root(const char *root);

#endif
