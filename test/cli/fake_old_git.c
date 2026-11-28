#include <stdio.h>
#include <string.h>

/** Stands in for a git older than naula takes: it answers its version and refuses everything else. */
int main(int count, char **values) {
    if (count == 2 && strcmp(values[1], "--version") == 0) {
        fputs("git version 2.19.0\n", stdout);
        return 0;
    }
    fputs("fake old git: only --version is answered\n", stderr);
    return 1;
}
