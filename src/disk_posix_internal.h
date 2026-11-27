#ifndef NAULA_DISK_POSIX_INTERNAL_H
#define NAULA_DISK_POSIX_INTERNAL_H

#include "naula/status.h"

/**
 * Flushes the folder holding `path`, so a name made, renamed or removed there survives a power cut. Out of memory is
 * answered after the change it follows was made, which the journal's missing `done` line leaves to a resume to
 * confirm.
 */
naula_status disk_sync_parent(const char *path);

#endif
