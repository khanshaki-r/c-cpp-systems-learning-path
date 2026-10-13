// m12_packed_record_decode.cpp : decoding packed legacy records byte by byte, with golden-record tests.
//
// Build and run (WSL Ubuntu 24.04, GCC 13):
//     g++ -std=c++17 -Wall -Wextra -Werror m12_packed_record_decode.cpp -o decode && ./decode
//
// The layout below is the one in module 12, section 2.2: written by a C compiler with
// #pragma pack(1) on x86, so integers are little-endian EXCEPT branch_no, which the old code wrote
// with htons() and is therefore big-endian. The decoder never casts the buffer to a struct
// pointer; it reads each field with explicit offsets and an explicit byte order.
//
// Exercise (module 12, Try it 2): change rd_be16 to rd_le16 in decode_v2() and re-run. The
// golden test for branch_no fails with 0xF401 = 62465 instead of 0x01F4 = 500: the two bytes
// are the same, only the order you combine them in differs.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// ------------------------------------------------------------------------------------------
// The historical struct, kept ONLY to document offsets and to check sizeof at compile time.
// We never read through it.
// ------------------------------------------------------------------------------------------
#pragma pack(push, 1)
struct loan_rec_v2 {
    std::uint8_t  version;          // 2
    char          loan_no[12];      // space-padded, not NUL-terminated
    std::uint32_t customer_id;      // little-endian
    std::int64_t  amount_cents;     // little-endian
    char          status;           // 'D','S','A','R','W'
    std::uint8_t  bcd_date[4];      // YYYYMMDD packed BCD
    std::uint16_t branch_no;        // BIG-endian (htons in the writer)
    char          officer[3];
};
#pragma pack(pop)
static_assert(sizeof(loan_rec_v2) == 35, "packed layout must be 35 bytes");
static_assert(offsetof(loan_rec_v2, customer_id) == 13, "offset check");
static_assert(offsetof(loan_rec_v2, amount_cents) == 17, "offset check");
static_assert(offsetof(loan_rec_v2, status) == 25, "offset check");
static_assert(offsetof(loan_rec_v2, bcd_date) == 26, "offset check");
static_assert(offsetof(loan_rec_v2, branch_no) == 30, "offset check");
static_assert(offsetof(loan_rec_v2, officer) == 32, "offset check");

// Without the pragma the compiler pads: customer_id moves to offset 16 and sizeof grows.
// This is why the phase-0 inventory must record the ORIGINAL compiler and pragmas.
struct loan_rec_v2_unpacked {
    std::uint8_t  version;
    char          loan_no[12];
    std::uint32_t customer_id;
    std::int64_t  amount_cents;
    char          status;
    std::uint8_t  bcd_date[4];
    std::uint16_t branch_no;
    char          officer[3];
};

// ------------------------------------------------------------------------------------------
// Byte-order readers. Cast each byte to the wide unsigned type BEFORE shifting.
// ------------------------------------------------------------------------------------------
static std::uint16_t rd_le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}
static std::uint16_t rd_be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}
static std::uint32_t rd_le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
static std::uint32_t rd_be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}
static std::int64_t rd_le64s(const std::uint8_t* p) {
    std::uint64_t u = 0;
    for (int i = 7; i >= 0; --i) u = (u << 8) | p[i];
    std::int64_t s;
    std::memcpy(&s, &u, sizeof s);              // reinterpret the bits as signed without UB
    return s;
}

// ------------------------------------------------------------------------------------------
// Decoded form: natural types, owned strings. Alignment of THIS struct is irrelevant.
// ------------------------------------------------------------------------------------------
struct Date { int y, m, d; };

struct Loan {
    std::string loan_no;
    std::uint32_t customer_id = 0;
    std::int64_t amount_cents = 0;
    char status = '?';
    std::optional<Date> date;
    std::uint16_t branch_no = 0;
    std::string officer;
};

static std::string trim_fixed(const std::uint8_t* p, std::size_t n) {
    std::string_view s(reinterpret_cast<const char*>(p), n);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.remove_suffix(1);
    return std::string(s);
}

// Returns false on an invalid nibble (a parity finding, not a crash). All-zero = no date.
static bool bcd_date(const std::uint8_t* p, std::optional<Date>& out) {
    if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0) { out.reset(); return true; }
    int dig[8];
    for (int i = 0; i < 4; ++i) {
        int hi = p[i] >> 4, lo = p[i] & 0x0F;
        if (hi > 9 || lo > 9) return false;
        dig[2 * i] = hi;
        dig[2 * i + 1] = lo;
    }
    out = Date{dig[0] * 1000 + dig[1] * 100 + dig[2] * 10 + dig[3], dig[4] * 10 + dig[5], dig[6] * 10 + dig[7]};
    return out->m >= 1 && out->m <= 12 && out->d >= 1 && out->d <= 31;
}

static std::optional<Loan> decode_v2(const std::uint8_t* p, std::size_t n, std::string& why) {
    if (n != sizeof(loan_rec_v2)) { why = "length " + std::to_string(n) + " != 35"; return std::nullopt; }
    if (p[0] != 2) { why = "version " + std::to_string(p[0]) + " != 2"; return std::nullopt; }
    Loan l;
    l.loan_no = trim_fixed(p + 1, 12);
    l.customer_id = rd_le32(p + 13);
    l.amount_cents = rd_le64s(p + 17);
    l.status = static_cast<char>(p[25]);
    if (!bcd_date(p + 26, l.date)) { why = "bad BCD date"; return std::nullopt; }
    l.branch_no = rd_be16(p + 30);              // big-endian field inside a little-endian record
    l.officer = trim_fixed(p + 32, 3);
    return l;
}

// ------------------------------------------------------------------------------------------
// Golden records: bytes copied from `db_dump -p` of real (here: hand-made) records and
// verified by hand with a hex editor. Each test pins one decoding rule.
// ------------------------------------------------------------------------------------------
static const std::uint8_t kGolden1[35] = {
    0x02,                                                       // version 2
    'L', 'N', '2', '0', '2', '4', '0', '0', '0', '1', '2', '3', // loan_no "LN2024000123"
    0x39, 0x30, 0x00, 0x00,                                     // customer_id 12345 LE (0x3039)
    0x40, 0xE2, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,             // amount 123456 cents LE (0x1E240)
    'S',                                                        // status
    0x20, 0x24, 0x03, 0x15,                                     // 2024-03-15 BCD
    0x01, 0xF4,                                                 // branch 500 BE (0x01F4)
    'K', '7', ' '                                               // officer "K7" padded
};

static const std::uint8_t kGoldenDraft[35] = {
    0x02,
    'L', 'N', '2', '0', '2', '3', '9', '9', '9', '9', '9', '9',
    0xFF, 0xFF, 0xFF, 0xFF,                                     // customer_id 4294967295: max u32
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F,             // amount INT64_MAX
    'D',
    0x00, 0x00, 0x00, 0x00,                                     // no date: draft
    0x00, 0x07,                                                 // branch 7
    0x00, 0x00, 0x00                                            // officer all NUL
};

static const std::uint8_t kGoldenBadBcd[35] = {
    0x02,
    'L', 'N', '2', '0', '2', '2', '0', '0', '0', '0', '0', '1',
    0x01, 0x00, 0x00, 0x00,
    0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    'A',
    0x20, 0x2A, 0x01, 0x01,                                     // 0x2A: nibble A is not a digit
    0x00, 0x01,
    'X', 'Y', 'Z'
};

static int g_run = 0, g_failed = 0;

#define EXPECT(cond, msg)                                                         \
    do {                                                                          \
        ++g_run;                                                                  \
        if (!(cond)) { ++g_failed; std::printf("FAIL: %s  (%s)\n", msg, #cond); } \
        else std::printf("ok:   %s\n", msg);                                      \
    } while (0)

int main() {
    std::string why;

    // 1. Field-by-field decode of a normal record.
    auto a = decode_v2(kGolden1, sizeof kGolden1, why);
    EXPECT(a.has_value(), "golden 1 decodes");
    if (a) {
        EXPECT(a->loan_no == "LN2024000123", "loan_no is the 12 raw chars");
        EXPECT(a->customer_id == 12345u, "customer_id little-endian");
        EXPECT(a->amount_cents == 123456, "amount_cents little-endian int64");
        EXPECT(a->status == 'S', "status char");
        EXPECT(a->date && a->date->y == 2024 && a->date->m == 3 && a->date->d == 15, "BCD date 2024-03-15");
        EXPECT(a->branch_no == 500, "branch_no big-endian (htons in the writer)");
        EXPECT(a->officer == "K7", "fixed-width officer code trimmed");
    }

    // 2. The same two bytes read the wrong way round: what a plain struct cast gives you on x86.
    EXPECT(rd_le16(kGolden1 + 30) == 0xF401, "reading branch_no as LE gives 62465: the classic bug");
    EXPECT(rd_be32(kGolden1 + 13) == 0x39300000u, "reading customer_id as BE gives 959447040");

    // 3. Edge values and the draft rule.
    auto d = decode_v2(kGoldenDraft, sizeof kGoldenDraft, why);
    EXPECT(d.has_value(), "draft decodes");
    if (d) {
        EXPECT(d->customer_id == 4294967295u, "max uint32 survives (no sign extension)");
        EXPECT(d->amount_cents == INT64_MAX, "INT64_MAX survives");
        EXPECT(!d->date.has_value(), "all-zero BCD means no date");
        EXPECT(d->officer.empty(), "NUL padding trimmed like space padding");
    }

    // 4. Invalid data is a finding, not a crash.
    auto b = decode_v2(kGoldenBadBcd, sizeof kGoldenBadBcd, why);
    EXPECT(!b.has_value() && why == "bad BCD date", "invalid BCD nibble rejected with a reason");
    EXPECT(!decode_v2(kGolden1, 30, why).has_value(), "truncated record rejected");

    // 5. Unaligned input: Berkeley DB hands back pointers into a page at arbitrary offsets.
    //    memcpy-style reads do not care; *(uint32_t*)(buf + 13) would be UB.
    std::vector<std::uint8_t> shifted(sizeof kGolden1 + 1);
    std::memcpy(shifted.data() + 1, kGolden1, sizeof kGolden1);
    auto u = decode_v2(shifted.data() + 1, sizeof kGolden1, why);
    EXPECT(u && u->customer_id == 12345u && u->branch_no == 500, "decode from an odd address");

    // 6. The padding trap: the same declaration without the pragma has a different layout.
    std::printf("info: sizeof packed = %zu, unpacked = %zu; customer_id offset packed = %zu, unpacked = %zu\n",
                sizeof(loan_rec_v2), sizeof(loan_rec_v2_unpacked),
                offsetof(loan_rec_v2, customer_id), offsetof(loan_rec_v2_unpacked, customer_id));
    EXPECT(sizeof(loan_rec_v2_unpacked) != sizeof(loan_rec_v2), "unpacked struct has padding");

    std::printf("\n%d/%d tests passed\n", g_run - g_failed, g_run);
    return g_failed == 0 ? 0 : 1;
}
