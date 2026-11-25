#ifndef NAULA_TRANSPORT_INTERNAL_H
#define NAULA_TRANSPORT_INTERNAL_H

#include "naula/transport.h"

/** Whether request is read as far as its first shape; a request that is not is answered REFUSED. */
int naula_fetch_request_is_usable(const naula_fetch_request *request, naula_fetch_answer *answer);

#endif
