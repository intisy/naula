#ifndef NAULA_HOME_INTERNAL_H
#define NAULA_HOME_INTERNAL_H

/** The checks naula_home_from_json and naula_place_parse apply, each to one whole text. */

int naula_home_is_provider(const char *text);
int naula_home_is_account(const char *text);
int naula_home_is_domain(const char *text);
int naula_home_is_host(const char *text);
int naula_home_is_git_url(const char *text);
int naula_home_is_numbered_place(const char *text);
int naula_home_is_web_place(const char *text);

/** Whether naula_home_from_json reads a member of this name. */
int naula_home_names_member(const char *name);

#endif
