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
#else
/**
 * Forgets any loaded libcurl and makes the next fetch open the shared object called library_name instead
 * of the platform's own; NULL restores the default.
 */
void naula_fetch_curl_use_library(const char *library_name);
#endif

#endif
