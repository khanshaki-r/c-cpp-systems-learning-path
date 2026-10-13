#include "minidb/recovery.hpp"

#include <utility>
#include <vector>

namespace minidb {

RecoveryStats Recovery::run(Pager& pager, Wal& wal) {
    RecoveryStats stats;
    stats.end_lsn = wal.start_lsn();

    // Page images of the transaction being read; applied only when its commit record shows
    // up. Images of a transaction whose commit never reached the disk are never applied.
    std::vector<std::pair<PageId, Page>> pending;
    Page current;

    stats.records = wal.replay([&](const WalRecord& record) {
        if (record.type == RecordType::PageImage) {
            pending.emplace_back(record.page_id, record.image);
            return;
        }
        // Commit: redo every image of this transaction, in log order.
        for (auto& [page_id, image] : pending) {
            // The page LSN check is an optimisation, not a correctness requirement: if the
            // data file already holds this image or a later one, rewriting is unnecessary.
            // A torn or missing page fails read_if_valid() and is always rewritten.
            if (pager.read_if_valid(page_id, current) && current.lsn() >= image.lsn()) {
                ++stats.pages_skipped;
                continue;
            }
            pager.write(page_id, image);
            ++stats.pages_redone;
        }
        pending.clear();
        ++stats.committed_txns;
        stats.end_lsn = record.lsn + Wal::kRecordHeaderSize + 8;  // just past the commit record
    });

    stats.discarded_records = pending.size();
    pager.sync();                  // redone pages durable before the log can be reset
    wal.truncate_at(stats.end_lsn);  // drop the uncommitted tail before anything is appended
    return stats;
}

}  // namespace minidb
