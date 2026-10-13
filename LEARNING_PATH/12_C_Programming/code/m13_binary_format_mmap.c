/* m13_binary_format_mmap.c - a binary file format with explicit byte order, a version, a
 * checksum and bounds-checked decoding, read back through mmap().
 *
 * Why not fwrite(&record, sizeof record, 1, f)? Because the bytes of a struct depend on the
 * compiler (padding), the CPU (endianness) and the build (sizeof long is 4 on Windows and
 * 8 on Linux). A file written by the Windows server would be garbage to the Linux tool.
 * Encode each field yourself, in a byte order you choose and document.
 *
 * File format "ACCT" version 1, all integers little-endian:
 *   offset  size  field
 *   0       4     magic "ACCT"
 *   4       2     version (1)
 *   6       2     header size (16)
 *   8       4     record count
 *   12      4     CRC-32 (IEEE) of every byte after the header
 *   16      ...   records: u64 id, i64 balance_paise, u8 name_len, name bytes (no NUL)
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m13_binary_format_mmap m13_binary_format_mmap.c
 * Run:    ./m13_binary_format_mmap
 * Look:   the program prints a hex dump of the first 48 bytes; compare it with the table above.
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

/* ---------------- byte-order helpers: shifts are endian-independent ---------------- */
static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* int64 <-> uint64 without implementation-defined conversion of out-of-range values. */
static uint64_t i64_to_u64(int64_t v) { return (uint64_t)v; }    /* always defined: mod 2^64 */
static int64_t u64_to_i64(uint64_t u)
{
    if (u <= (uint64_t)INT64_MAX)
        return (int64_t)u;
    return -(int64_t)(UINT64_MAX - u) - 1;
}

/* CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320), bit by bit: slow but obviously right. */
static uint32_t crc32_ieee(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* ---------------- the record and the codec ---------------- */
struct account {
    uint64_t id;
    int64_t  balance_paise;
    char     name[64];
};

enum { ACCT_OK = 0, ACCT_ETRUNC, ACCT_EMAGIC, ACCT_EVERSION, ACCT_ECRC, ACCT_ENAME, ACCT_ECOUNT };
#define HDR_SIZE 16u

/* Encode into buf (caller sizes it). Returns bytes used. */
static size_t encode(const struct account *a, uint32_t n, uint8_t *buf)
{
    size_t off = HDR_SIZE;
    for (uint32_t i = 0; i < n; i++) {
        size_t nl = strlen(a[i].name);
        assert(nl <= 255);
        put_u64(buf + off, a[i].id);                       off += 8;
        put_u64(buf + off, i64_to_u64(a[i].balance_paise)); off += 8;
        buf[off++] = (uint8_t)nl;
        memcpy(buf + off, a[i].name, nl);                  off += nl;
    }
    memcpy(buf, "ACCT", 4);
    put_u16(buf + 4, 1);
    put_u16(buf + 6, HDR_SIZE);
    put_u32(buf + 8, n);
    put_u32(buf + 12, crc32_ieee(buf + HDR_SIZE, off - HDR_SIZE));
    return off;
}

/* Decode with every length checked against the bytes actually present: the file is input,
 * and input can be truncated, corrupted or hostile. */
static int decode(const uint8_t *p, size_t len, struct account *out, uint32_t max, uint32_t *n_out)
{
    if (len < HDR_SIZE) return ACCT_ETRUNC;
    if (memcmp(p, "ACCT", 4) != 0) return ACCT_EMAGIC;
    if (get_u16(p + 4) != 1) return ACCT_EVERSION;
    size_t hsize = get_u16(p + 6);
    if (hsize < HDR_SIZE || hsize > len) return ACCT_ETRUNC;
    uint32_t n = get_u32(p + 8);
    if (crc32_ieee(p + hsize, len - hsize) != get_u32(p + 12)) return ACCT_ECRC;
    if (n > max) return ACCT_ECOUNT;

    size_t off = hsize;
    for (uint32_t i = 0; i < n; i++) {
        if (len - off < 17) return ACCT_ETRUNC;
        out[i].id = get_u64(p + off);                         off += 8;
        out[i].balance_paise = u64_to_i64(get_u64(p + off));  off += 8;
        size_t nl = p[off++];
        if (nl >= sizeof out[i].name) return ACCT_ENAME;
        if (len - off < nl) return ACCT_ETRUNC;
        memcpy(out[i].name, p + off, nl);
        out[i].name[nl] = '\0';
        off += nl;
    }
    *n_out = n;
    return ACCT_OK;
}

static void hexdump(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i += 16) {
        printf("%08zx ", i);
        for (size_t j = i; j < i + 16 && j < n; j++)
            printf(" %02x", p[j]);
        printf("\n");
    }
}

int main(void)
{
    assert(crc32_ieee((const uint8_t *)"123456789", 9) == 0xCBF43926u);   /* standard check value */
    assert(u64_to_i64(i64_to_u64(INT64_MIN)) == INT64_MIN);
    assert(u64_to_i64(i64_to_u64(-1)) == -1);

    struct account in[3] = {
        { 1001, 25000000, "Asha Rao" },
        { 1002, -150, "Bilal Khan" },
        { 1003, INT64_MAX, "Treasury" },
    };
    uint8_t buf[512];
    size_t len = encode(in, 3, buf);

    /* write it, then map it back */
    char path[] = "/tmp/m13_acct_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    ssize_t w = write(fd, buf, len);
    assert(w == (ssize_t)len);
    close(fd);

    fd = open(path, O_RDONLY);
    assert(fd >= 0);
    struct stat st;
    int rc = fstat(fd, &st);
    assert(rc == 0 && (size_t)st.st_size == len);
    /* mmap: the file's pages appear in our address space; reading them faults them in from
     * the page cache. No read() calls, no copy into a user buffer. */
    const uint8_t *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(map != MAP_FAILED);
    close(fd);                           /* the mapping stays valid after close */

    printf("file is %zu bytes; first 48:\n", len);
    hexdump(map, 48);

    struct account out[8];
    uint32_t n = 0;
    rc = decode(map, (size_t)st.st_size, out, 8, &n);
    assert(rc == ACCT_OK && n == 3);
    for (uint32_t i = 0; i < n; i++) {
        assert(out[i].id == in[i].id && out[i].balance_paise == in[i].balance_paise);
        assert(strcmp(out[i].name, in[i].name) == 0);
        printf("  id=%llu balance=%lld name=%s\n", (unsigned long long)out[i].id,
               (long long)out[i].balance_paise, out[i].name);
    }
    munmap((void *)map, (size_t)st.st_size);
    unlink(path);

    /* damaged inputs are rejected with a specific reason, never decoded into garbage */
    uint8_t bad[512];
    memcpy(bad, buf, len);
    bad[20] ^= 0x01;                                          /* one flipped bit */
    assert(decode(bad, len, out, 8, &n) == ACCT_ECRC);
    assert(decode(buf, len - 5, out, 8, &n) == ACCT_ECRC);    /* truncated: CRC catches it */
    assert(decode(buf, 10, out, 8, &n) == ACCT_ETRUNC);
    memcpy(bad, buf, len);
    bad[0] = 'X';
    assert(decode(bad, len, out, 8, &n) == ACCT_EMAGIC);
    memcpy(bad, buf, len);
    put_u16(bad + 4, 2);
    assert(decode(bad, len, out, 8, &n) == ACCT_EVERSION);
    assert(decode(buf, len, out, 2, &n) == ACCT_ECOUNT);      /* more records than room */
    puts("corrupt, truncated, wrong-magic and wrong-version inputs all rejected");
    puts("all checks passed");
    return 0;
}
