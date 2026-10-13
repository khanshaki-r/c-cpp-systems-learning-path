#pragma once

#include <cstddef>

#include "minidb/pager.hpp"
#include "minidb/wal.hpp"

namespace minidb {

struct RecoveryStats {
    std::size_t records = 0;              // valid records found in the log
    std::size_t committed_txns = 0;       // transactions with a commit record
    std::size_t pages_redone = 0;         // page images written to the data file
    std::size_t pages_skipped = 0;        // data file already had this image or a newer one
    std::size_t discarded_records = 0;    // records of a transaction that never committed
    Lsn end_lsn = 0;                      // log end after recovery (just past the last commit)
};

// Redo-only crash recovery.
//
// Why redo-only works: the PageCache never writes a page modified by an uncommitted
// transaction to the data file (no-steal), so the data file never needs undoing. It may,
// however, be missing committed changes (no-force: commit only syncs the log) and may hold a
// torn page (crash during a page write). Every committed change is in the log as a full page
// image, so replaying committed images in log order repairs both.
//
// Idempotent: running it twice, or crashing during it and running it again, gives the same
// result, because applying a full page image is idempotent and replay is in LSN order.
class Recovery {
public:
    static RecoveryStats run(Pager& pager, Wal& wal);
};

}  // namespace minidb
