#include <stdint.h>
#include <string.h>

#include "check.h"
#include "naula/sha256.h"

static void expect_digest(const char *message, const char *expected, const char *label) {
    naula_sha256 hash;
    char hex[NAULA_SHA256_HEX_SIZE];
    naula_sha256_start(&hash);
    naula_sha256_add(&hash, (const uint8_t *)message, strlen(message));
    naula_sha256_hex(&hash, hex);
    check_text(hex, expected, label);
}

static void sha256_matches_fips_vectors(void) {
    expect_digest("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty");
    expect_digest("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
    expect_digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "448-bit");
}

static void a_million_a_fed_in_uneven_pieces_matches_fips(void) {
    uint8_t piece[997];
    memset(piece, 'a', sizeof piece);
    naula_sha256 hash;
    naula_sha256_start(&hash);
    size_t remaining = 1000000;
    while (remaining > 0) {
        size_t length = remaining < sizeof piece ? remaining : sizeof piece;
        naula_sha256_add(&hash, piece, length);
        remaining -= length;
    }
    char hex[NAULA_SHA256_HEX_SIZE];
    naula_sha256_hex(&hash, hex);
    check_text(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "million a");
}

static void finish_answers_the_raw_digest_hex_spells(void) {
    static const uint8_t abc_digest_start[4] = {0xba, 0x78, 0x16, 0xbf};
    uint8_t digest[NAULA_SHA256_SIZE];
    naula_sha256 hash;
    naula_sha256_start(&hash);
    naula_sha256_add(&hash, (const uint8_t *)"abc", 3);
    naula_sha256_finish(&hash, digest);
    CHECK(memcmp(digest, abc_digest_start, sizeof abc_digest_start) == 0);
    CHECK(digest[NAULA_SHA256_SIZE - 1] == 0xad);
}

int main(void) {
    sha256_matches_fips_vectors();
    a_million_a_fed_in_uneven_pieces_matches_fips();
    finish_answers_the_raw_digest_hex_spells();
    return check_summary("sha256_test");
}
