#ifndef NAULA_TRANSPORT_HOOKS_H
#define NAULA_TRANSPORT_HOOKS_H

/**
 * Lets the transport take plain http: urls, which only a loopback test fixture serves, since the fixture carries no
 * TLS of its own.
 *
 * @implNote Defined only when NAULA_TEST_HOOKS is set, which only the naula_test_hooks library does; the shipped
 * library carries no such symbol and refuses http: always.
 */
void naula_fetch_allow_plain_http(int allowed);

/** Replaces the deadline of every fetch; 0 restores the caller's own. */
void naula_fetch_use_deadline(unsigned milliseconds);

#if defined(_WIN32)
/** The WinHTTP access type every session is opened with. */
unsigned long naula_fetch_windows_access_type(void);

/**
 * Closes each request's handle as the deadline would, at point: NAULA_FETCH_CLOSE_AFTER_RECEIVE once the answer's head
 * arrived and before it is read, NAULA_FETCH_CLOSE_AFTER_EXCHANGE once the answer is settled; 0 never.
 */
void naula_fetch_windows_close_at(int point);

#define NAULA_FETCH_CLOSE_AFTER_RECEIVE 1
#define NAULA_FETCH_CLOSE_AFTER_EXCHANGE 2
#else
/**
 * Forgets any loaded libcurl and makes the next fetch open the shared object called library_name instead
 * of the platform's own; NULL restores the default.
 */
void naula_fetch_curl_use_library(const char *library_name);
#endif

#endif
