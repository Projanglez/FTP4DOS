/* =============================================================================
 * crypttest.cpp - test harness for src/sha256.cpp and src/rsaverify.cpp
 * -----------------------------------------------------------------------------
 * Builds natively (wcl386 -bt=nt) as well as for DOS. Both modules use pinned
 * integer widths, so a host run exercises the same arithmetic the 386 will do.
 *
 *   crypttest selftest
 *       SHA-256 against the FIPS 180-4 vectors, plus chunked-feed and
 *       hex round-trip checks.
 *
 *   crypttest verify <modulus.bin> <sig.bin> <signed-file>
 *       Hashes <signed-file>, verifies <sig.bin> against <modulus.bin>.
 *       Prints VALID or INVALID and exits 0 / 1. Driven by tools/test-crypto.sh,
 *       which produces the inputs with OpenSSL.
 *
 * No test key material lives in this repository: the driver generates a
 * throwaway key on the fly.
 * ===========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/sha256.h"
#include "../src/rsaverify.h"
#include "crypttest_vec.h"

static int g_fail = 0;

static void check(const char *what, int ok)
{
    printf("  %-46s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) g_fail++;
}

static int hash_equals(const unsigned char *h, const char *expect_hex)
{
    char got[SHA256_HEX_LEN];
    sha256_hex(h, got);
    return strcmp(got, expect_hex) == 0;
}

/* 0 = quick. The exhaustive parts (a million-byte hash, 256 signature
 * verifications) take minutes on a real 386 and prove nothing the short
 * versions do not, so the hardware run skips them by default. */
static int g_thorough = 1;

static void test_sha256_vectors(void)
{
    unsigned char h[SHA256_DIGEST_LEN];
    struct Sha256Ctx c;
    unsigned long i;

    printf("SHA-256 (FIPS 180-4 vectors)\n");

    sha256_buf("", 0, h);
    check("empty string",
          hash_equals(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

    sha256_buf("abc", 3, h);
    check("\"abc\"",
          hash_equals(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    /* 448 bits: forces the length to spill into a second block. */
    sha256_buf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, h);
    check("two-block message (448 bit)",
          hash_equals(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    /* 896 bits. */
    sha256_buf("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
               "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", 112, h);
    check("multi-block message (896 bit)",
          hash_equals(h, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));

    /* One million 'a': exercises the 64-bit length counter past 2^23 bits.
     * Minutes on a 386, so quick mode uses a 100 KB message instead - still
     * past the 16-bit boundary where the length counter could go wrong. */
    if (g_thorough) {
        sha256_init(&c);
        for (i = 0; i < 1000000UL; i++) sha256_update(&c, "a", 1);
        sha256_final(&c, h);
        check("one million 'a'",
              hash_equals(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    } else {
        sha256_init(&c);
        for (i = 0; i < 100000UL; i++) sha256_update(&c, "a", 1);
        sha256_final(&c, h);
        check("100000 x 'a'",
              hash_equals(h, "6d1cf22d7cc09b085dfc25ee1a1f3ae0265804c607bc2074ad253bcc82fd81ee"));
    }
}

static void test_sha256_chunking(void)
{
    /* Same message fed in awkward chunk sizes must give the same digest - this
     * is what the buffering in sha256_update() has to get right. */
    static const char msg[] =
        "The quick brown fox jumps over the lazy dog. Pack my box with five "
        "dozen liquor jugs. How vexingly quick daft zebras jump! 0123456789";
    unsigned int len = (unsigned int)strlen(msg);
    unsigned char ref[SHA256_DIGEST_LEN], got[SHA256_DIGEST_LEN];
    struct Sha256Ctx c;
    unsigned int step, off;
    int ok = 1;

    printf("SHA-256 (chunked feeding)\n");
    sha256_buf(msg, len, ref);

    for (step = 1; step <= 70; step++) {
        sha256_init(&c);
        for (off = 0; off < len; off += step) {
            unsigned int n = (len - off < step) ? (len - off) : step;
            sha256_update(&c, msg + off, n);
        }
        sha256_final(&c, got);
        if (memcmp(ref, got, SHA256_DIGEST_LEN) != 0) { ok = 0; break; }
    }
    check("identical digest for chunk sizes 1..70", ok);
}

static void test_hex(void)
{
    unsigned char h[SHA256_DIGEST_LEN], back[SHA256_DIGEST_LEN];
    char hex[SHA256_HEX_LEN];
    int i;

    printf("hex helpers\n");
    for (i = 0; i < SHA256_DIGEST_LEN; i++) h[i] = (unsigned char)(i * 7 + 3);
    sha256_hex(h, hex);
    check("hex round-trip",
          sha256_parse_hex(hex, back) == 0 &&
          memcmp(h, back, SHA256_DIGEST_LEN) == 0);
    check("uppercase accepted",
          sha256_parse_hex("ABCDEF0123456789ABCDEF0123456789"
                           "ABCDEF0123456789ABCDEF0123456789", back) == 0);
    check("short string rejected", sha256_parse_hex("abcd", back) != 0);
    check("non-hex rejected",
          sha256_parse_hex("zzcdef0123456789abcdef0123456789"
                           "abcdef0123456789abcdef0123456789", back) != 0);
    check("trailing junk rejected",
          sha256_parse_hex("abcdef0123456789abcdef0123456789"
                           "abcdef0123456789abcdef0123456789x", back) != 0);
}

/* Self-contained RSA checks against the compiled-in vector. This is the mode
 * that matters on DOS: it needs no files, so it can be run straight from the
 * VM's C: prompt and proves the 16-bit arithmetic agrees with OpenSSL. */
static void test_rsa_vectors(void)
{
    unsigned char hash[SHA256_DIGEST_LEN];
    unsigned char sig[RSA_BYTES];
    unsigned char mod[RSA_BYTES];
    unsigned int msglen = (unsigned int)(sizeof(TESTVEC_MSG) - 1);
    int i;

    printf("RSA-2048 (compiled-in vector)\n");

    sha256_buf(TESTVEC_MSG, msglen, hash);
    memcpy(sig, TESTVEC_SIG, RSA_BYTES);
    memcpy(mod, TESTVEC_MODULUS, RSA_BYTES);

    check("genuine signature",
          rsa_verify_sha256(sig, RSA_BYTES, hash, mod) == 1);

    check("signature from a different key",
          rsa_verify_sha256(sig, RSA_BYTES, hash, TESTVEC_MODULUS_OTHER) == 0);

    /* Every single-bit change anywhere in the signature must be fatal. Sampling
     * one byte would not show a reduction bug that only bites in a few limbs -
     * hence all 256 in thorough mode. Each verification is a full modexp, which
     * on a 386 means minutes for the whole sweep, so quick mode takes a spread
     * of eight: first, last, and the limb boundaries in between. */
    {
        int allrejected = 1;
        if (g_thorough) {
            for (i = 0; i < RSA_BYTES; i++) {
                sig[i] = (unsigned char)(sig[i] ^ 0x01);
                if (rsa_verify_sha256(sig, RSA_BYTES, hash, mod) != 0) allrejected = 0;
                sig[i] = (unsigned char)(sig[i] ^ 0x01);
            }
            check("all 256 single-bit signature flips rejected", allrejected);
        } else {
            static const int spots[8] = { 0, 1, 31, 63, 127, 200, 254, 255 };
            int k;
            for (k = 0; k < 8; k++) {
                i = spots[k];
                sig[i] = (unsigned char)(sig[i] ^ 0x01);
                if (rsa_verify_sha256(sig, RSA_BYTES, hash, mod) != 0) allrejected = 0;
                sig[i] = (unsigned char)(sig[i] ^ 0x01);
            }
            check("8 single-bit signature flips rejected", allrejected);
        }
    }

    /* Same for the message. */
    {
        unsigned char h2[SHA256_DIGEST_LEN];
        memcpy(h2, hash, SHA256_DIGEST_LEN);
        h2[0] = (unsigned char)(h2[0] ^ 0x01);
        check("altered message hash rejected",
              rsa_verify_sha256(TESTVEC_SIG, RSA_BYTES, h2, mod) == 0);
    }

    {
        unsigned char z[RSA_BYTES];
        memset(z, 0, sizeof(z));
        check("s = 0", rsa_verify_sha256(z, RSA_BYTES, hash, mod) == 0);
        z[RSA_BYTES - 1] = 1;
        check("s = 1", rsa_verify_sha256(z, RSA_BYTES, hash, mod) == 0);
    }

    check("s = n", rsa_verify_sha256(TESTVEC_MODULUS, RSA_BYTES, hash, mod) == 0);

    check("wrong length (255)",
          rsa_verify_sha256(TESTVEC_SIG, RSA_BYTES - 1, hash, mod) == 0);
    check("wrong length (257)",
          rsa_verify_sha256(TESTVEC_SIG, RSA_BYTES + 1, hash, mod) == 0);
    check("wrong length (0)",
          rsa_verify_sha256(TESTVEC_SIG, 0, hash, mod) == 0);

    /* A modulus with a clear top bit or an even value breaks the assumptions
     * behind R mod n = R - n and behind Montgomery reduction. Both must be
     * refused rather than silently miscomputed. */
    {
        unsigned char bad[RSA_BYTES];
        memcpy(bad, TESTVEC_MODULUS, RSA_BYTES);
        bad[0] = (unsigned char)(bad[0] & 0x7F);
        check("modulus with clear top bit refused",
              rsa_verify_sha256(TESTVEC_SIG, RSA_BYTES, hash, bad) == 0);
        memcpy(bad, TESTVEC_MODULUS, RSA_BYTES);
        bad[RSA_BYTES - 1] = (unsigned char)(bad[RSA_BYTES - 1] & 0xFE);
        check("even modulus refused",
              rsa_verify_sha256(TESTVEC_SIG, RSA_BYTES, hash, bad) == 0);
    }
}

/* Timing, for the real-hardware run. The updater's cost budget is one RSA
 * verification per check plus one SHA-256 pass over the downloaded executable,
 * and both need to stay comfortable on a 25 MHz 386. clock() is good enough
 * here: the DOS tick is 54.9 ms and every measurement below runs for seconds. */
static void cmd_bench(void)
{
    unsigned char hash[SHA256_DIGEST_LEN];
    unsigned char *blob;
    struct Sha256Ctx c;
    clock_t t0, t1;
    unsigned int msglen = (unsigned int)(sizeof(TESTVEC_MSG) - 1);
    int i, reps;
    double secs;

    printf("benchmark (CLOCKS_PER_SEC = %ld)\n", (long)CLOCKS_PER_SEC);

    sha256_buf(TESTVEC_MSG, msglen, hash);

    reps = 8;
    t0 = clock();
    for (i = 0; i < reps; i++)
        rsa_verify_sha256(TESTVEC_SIG, RSA_BYTES, hash, TESTVEC_MODULUS);
    t1 = clock();
    secs = (double)(t1 - t0) / (double)CLOCKS_PER_SEC;
    printf("  rsa_verify_sha256   %6.3f s each  (%d reps, %.2f s total)\n",
           secs / reps, reps, secs);

    /* 256 KB in 8 KB pieces, close to the size of the shipped executable. */
    blob = (unsigned char *)malloc(8192);
    if (!blob) { printf("  (out of memory for the SHA-256 benchmark)\n"); return; }
    for (i = 0; i < 8192; i++) blob[i] = (unsigned char)i;

    t0 = clock();
    sha256_init(&c);
    for (i = 0; i < 32; i++) sha256_update(&c, blob, 8192);
    sha256_final(&c, hash);
    t1 = clock();
    secs = (double)(t1 - t0) / (double)CLOCKS_PER_SEC;
    printf("  sha256 over 256 KB  %6.3f s        (%.1f KB/s)\n",
           secs, secs > 0.0 ? 256.0 / secs : 0.0);
    free(blob);
}

static long read_file(const char *path, unsigned char *buf, long maxlen)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return -1;
    n = fread(buf, 1, (size_t)maxlen, f);
    fclose(f);
    return (long)n;
}

static int cmd_verify(const char *modpath, const char *sigpath,
                      const char *datapath)
{
    unsigned char modulus[RSA_BYTES], sig[RSA_BYTES + 8];
    unsigned char hash[SHA256_DIGEST_LEN];
    long nmod, nsig;
    int rc;

    nmod = read_file(modpath, modulus, (long)sizeof(modulus));
    if (nmod != RSA_BYTES) {
        fprintf(stderr, "modulus: expected %d bytes, got %ld\n", RSA_BYTES, nmod);
        return 2;
    }
    nsig = read_file(sigpath, sig, (long)sizeof(sig));
    if (nsig < 0) { fprintf(stderr, "cannot read %s\n", sigpath); return 2; }

    if (sha256_file(datapath, hash, 0, 0) != 0) {
        fprintf(stderr, "cannot hash %s\n", datapath);
        return 2;
    }

    rc = rsa_verify_sha256(sig, (unsigned int)nsig, hash, modulus);
    printf("%s\n", rc ? "VALID" : "INVALID");
    return rc ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && (strcmp(argv[1], "selftest") == 0 ||
                      strcmp(argv[1], "quick") == 0)) {
        g_thorough = (strcmp(argv[1], "selftest") == 0);
        if (!g_thorough) printf("quick mode - see 'selftest' for the exhaustive run\n\n");
        test_sha256_vectors();
        test_sha256_chunking();
        test_hex();
        test_rsa_vectors();
        printf("\n%s\n", g_fail ? "SELFTEST FAILED" : "selftest passed");
        return g_fail ? 1 : 0;
    }
    if (argc >= 2 && strcmp(argv[1], "bench") == 0) {
        cmd_bench();
        return 0;
    }
    /* Prints the SHA-256 of a file, so the DOS build's sha256_file() can be
     * compared against the host's. It is a separate code path from
     * sha256_buf(): binary mode, fseek/ftell sizing and chunked reads all
     * behave differently under DOS. */
    if (argc == 3 && strcmp(argv[1], "hash") == 0) {
        unsigned char h[SHA256_DIGEST_LEN];
        char hex[SHA256_HEX_LEN];
        int rc = sha256_file(argv[2], h, 0, 0);
        if (rc != 0) { fprintf(stderr, "cannot hash %s (rc=%d)\n", argv[2], rc); return 2; }
        sha256_hex(h, hex);
        printf("%s\n", hex);
        return 0;
    }
    if (argc == 5 && strcmp(argv[1], "verify") == 0)
        return cmd_verify(argv[2], argv[3], argv[4]);

    fprintf(stderr,
            "usage: crypttest quick     short correctness run (use this on a 386)\n"
            "       crypttest selftest  exhaustive run (minutes on period hardware)\n"
            "       crypttest bench     timing for RSA verify and SHA-256\n"
            "       crypttest hash <file>\n"
            "       crypttest verify <modulus.bin> <sig.bin> <signed-file>\n");
    return 2;
}
