/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>
#include <unordered_set>
#include <utility>

#include "common/common.h"
#include "index/ix.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"


namespace {
void close_fd_or_throw(int fd) {
    if (close(fd) < 0) {
        throw UnixError();
    }
}

void sync_fd_or_throw(int fd) {
    while (fdatasync(fd) < 0) {
        if (errno == EINTR) {
            continue;
        }
        throw UnixError();
    }
}

void sync_current_directory() {
    int dir_fd = open(".", O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0) {
        throw UnixError();
    }
    try {
        sync_fd_or_throw(dir_fd);
        close_fd_or_throw(dir_fd);
    } catch (...) {
        close(dir_fd);
        throw;
    }
}

void write_restart_file(const RestartRecord &record) {
    const std::string temp_path = DB_RESTART_NAME + ".tmp";
    int fd = open(temp_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        throw UnixError();
    }

    const char *data = reinterpret_cast<const char *>(&record);
    try {
        int written = 0;
        while (written < static_cast<int>(sizeof(record))) {
            ssize_t bytes = write(fd, data + written, sizeof(record) - written);
            if (bytes < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw UnixError();
            }
            if (bytes == 0) {
                throw UnixError();
            }
            written += static_cast<int>(bytes);
        }

        sync_fd_or_throw(fd);
        close_fd_or_throw(fd);
    } catch (...) {
        close(fd);
        unlink(temp_path.c_str());
        throw;
    }

    if (rename(temp_path.c_str(), DB_RESTART_NAME.c_str()) < 0) {
        unlink(temp_path.c_str());
        throw UnixError();
    }
    sync_current_directory();
}

//--------------------------------------------------------------------------
// Timestamp / transaction ID utilities
//--------------------------------------------------------------------------
auto is_uncommitted_ts(timestamp_t ts) -> bool {
    return ts >= TXN_START_ID;
}

auto make_txn_write_ts(Transaction *txn) -> timestamp_t {
    return TXN_START_ID + txn->get_transaction_id();
}

auto txn_id_from_write_ts(timestamp_t ts) -> txn_id_t {
    return ts - TXN_START_ID;
}

//-------------------------------------------------------------------------
// SSI visibility helper.
//-------------------------------------------------------------------------
auto ssi_writer_visible_to(Transaction *writer, Transaction *reader) -> bool {
    if (writer == nullptr || reader == nullptr) {
        return false;
    }
    if (writer->get_transaction_id() == reader->get_transaction_id()) {
        return true;
    }
    if (writer->get_state() == TransactionState::COMMITTED) {
        timestamp_t commit_ts = writer->get_commit_ts();
        return commit_ts != INVALID_TS && commit_ts <= reader->get_start_ts();
    }
    return false;
}
//--------------------------------------------------------------------------
// SSI record value / condition matching
//--------------------------------------------------------------------------

auto ssi_write_matches_conditions(const TabMeta &tab, const SsiWriteRecord &write,
                                  const std::vector<Condition> &conditions) -> bool {
    if (write.has_old_record_ && evaluate_conditions(conditions, write.old_record_, tab.cols)) {
        return true;
    }
    return write.has_new_record_ && evaluate_conditions(conditions, write.new_record_, tab.cols);
}

//--------------------------------------------------------------------------
// GC helper functions
//--------------------------------------------------------------------------

auto gc_version_safe_to_reclaim(const VersionUndoLink &version, timestamp_t watermark,
                                bool has_active_readers) -> bool {
    if (version.in_progress_) {
        return false;
    }
    if (version.ts_ == INVALID_TS) {
        return false;
    }
    if (!has_active_readers) {
        return true;
    }
    return version.ts_ < watermark;
}

auto checkpoint_version_safe_to_reclaim(const VersionUndoLink &version) -> bool {
    return !version.in_progress_ && version.ts_ != INVALID_TS;
}

void collect_referenced_txns_from_chain(const VersionUndoLink &version, std::unordered_set<txn_id_t> *referenced_txns,
                                        TransactionManager *txn_manager) {
    UndoLink undo_link = version.prev_;
    while (undo_link.IsValid()) {
        referenced_txns->insert(undo_link.prev_txn_);
        auto undo_log = txn_manager->GetUndoLogOptional(undo_link);
        if (!undo_log.has_value()) {
            break;
        }
        undo_link = undo_log->prev_version_;
    }
}

}  // namespace

//--------------------------------------------------------------------------
// Resource cleanup helpers
//--------------------------------------------------------------------------

void delete_write_set(Transaction *txn) {
    for (auto *write_record : *txn->get_write_set()) {
        delete write_record;
    }
    txn->get_write_set()->clear();
}

void release_locks(Transaction *txn, LockManager *lock_manager) {
    if (txn == nullptr || lock_manager == nullptr) {
        return;
    }
    std::vector<LockDataId> lock_ids(txn->get_lock_set()->begin(), txn->get_lock_set()->end());
    for (const auto &lock_id : lock_ids) {
        lock_manager->unlock(txn, lock_id);
    }
    txn->get_lock_set()->clear();
}

//--------------------------------------------------------------------------
// File handle, index, and CLR helpers
//--------------------------------------------------------------------------

auto find_table_by_fd_unlocked(SmManager *sm_manager, int fd) -> std::pair<std::string, RmFileHandle *> {
    if (sm_manager == nullptr) {
        return {std::string(), nullptr};
    }
    for (auto &[tab_name, fh] : sm_manager->fhs_) {
        if (fh != nullptr && fh->GetFd() == fd) {
            return {tab_name, fh.get()};
        }
    }
    return {std::string(), nullptr};
}

/**
 * Represents a delete record for garbage collection.
 */
struct GcDeleteRecord {
    int fd;
    Rid rid;
};

/**
 * Represents a version record for garbage collection, used to clean up version links after transactions commit or abort.
 */
struct GcVersionRecord {
    int fd;
    Rid rid;
    VersionUndoLink version;
};

/**
 * Represents a stale index record for garbage collection.
 */
struct GcStaleIndexRecord {
    int fd;
    Rid rid;
    RmRecord record;
    bool row_deleted;
};

enum class IndexEntryOp { INSERT, DELETE };

void apply_index_op(SmManager *sm_manager, const std::string &tab_name, const TabMeta &tab, const RmRecord &record,
                    const Rid &rid, Transaction *txn, IndexEntryOp op) {
    for (auto &index : tab.indexes) {
        auto ih = sm_manager->get_ih(tab_name, index.cols);
        auto key = std::make_unique<char[]>(index.col_tot_len);
        index.build_key(key.get(), record.data);
        if (op == IndexEntryOp::INSERT) {
            ih->insert_entry(key.get(), rid, txn);
        } else {
            ih->delete_entry(key.get(), rid, txn);
        }
    }
}

template <typename ClrRecord>
void append_txn_clr(Transaction *txn, LogManager *log_manager, ClrRecord &clr, LogType clr_type,
                    lsn_t undo_next_lsn) {
    if (log_manager == nullptr) {
        return;
    }
    clr.log_type_ = clr_type;
    clr.prev_lsn_ = txn->get_prev_lsn();
    clr.undo_next_lsn_ = undo_next_lsn;
    lsn_t lsn = log_manager->add_log_to_buffer(&clr);
    txn->set_prev_lsn(lsn);
}

//--------------------------------------------------------------------------
// Transaction Table Operations — locked txn registry
//--------------------------------------------------------------------------

TransactionManager::~TransactionManager() {
    StopGarbageCollectionWorker();
}

void TransactionManager::StartGarbageCollectionWorker() {
    gc_worker_ = std::thread(&TransactionManager::GarbageCollectionWorkerLoop, this);
}

void TransactionManager::StopGarbageCollectionWorker() {
    {
        std::lock_guard<std::mutex> lock(gc_worker_mutex_);
        gc_worker_stop_ = true;
    }
    gc_worker_cv_.notify_all();
    if (gc_worker_.joinable()) {
        gc_worker_.join();
    }
}

bool TransactionManager::IsGarbageCollectionWorkerStopped() {
    std::lock_guard<std::mutex> lock(gc_worker_mutex_);
    return gc_worker_stop_ && !gc_worker_.joinable();
}

TransactionManager::GarbageCollectionStats TransactionManager::GetGarbageCollectionStats() {
    GarbageCollectionStats stats;
    stats.requests = gc_counters_.requests.load(std::memory_order_acquire);
    stats.attempts = gc_counters_.attempts.load(std::memory_order_acquire);
    stats.worker_wakeups = gc_counters_.worker_wakeups.load(std::memory_order_acquire);
    stats.reclaimed_versions = gc_counters_.reclaimed_versions.load(std::memory_order_acquire);
    stats.stale_index_deletes = gc_counters_.stale_index_deletes.load(std::memory_order_acquire);
    stats.physical_deletes = gc_counters_.physical_deletes.load(std::memory_order_acquire);
    stats.retained_txn_releases = gc_counters_.retained_txn_releases.load(std::memory_order_acquire);
    stats.throttle_waits = gc_counters_.throttle_waits.load(std::memory_order_acquire);
    stats.worker_exits = gc_counters_.worker_exits.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(gc_worker_mutex_);
        stats.pending_requests = gc_counters_.pending_requests;
    }
    return stats;
}

// just request, add to pending count and wake up worker if needed
void TransactionManager::RequestGarbageCollection() {
    gc_counters_.requests.fetch_add(1, std::memory_order_acq_rel);
    bool notify_worker = false;
    {
        std::lock_guard<std::mutex> lock(gc_worker_mutex_);
        ++gc_counters_.pending_requests;
        notify_worker = gc_counters_.pending_requests >= kGarbageCollectionRequestThreshold;
    }
    if (notify_worker) {
        gc_worker_cv_.notify_one();
    }
}

void TransactionManager::GarbageCollectionWorkerLoop() {
    std::unique_lock<std::mutex> lock(gc_worker_mutex_);
    while (!gc_worker_stop_) {
        if (gc_counters_.pending_requests == 0) {
            gc_worker_cv_.wait_for(lock, std::chrono::milliseconds(kGarbageCollectionWorkerIntervalMs), [&] {
                return gc_worker_stop_ || gc_counters_.pending_requests >= kGarbageCollectionRequestThreshold;
            });
        }
        if (gc_worker_stop_) {
            break;
        }
        if (gc_counters_.pending_requests == 0) {
            continue;
        }
        gc_counters_.pending_requests = 0;
        gc_counters_.worker_wakeups.fetch_add(1, std::memory_order_acq_rel);
        lock.unlock();
        RunGarbageCollection(true);
        lock.lock();
    }
    gc_counters_.worker_exits.fetch_add(1, std::memory_order_acq_rel);
}

Transaction *TransactionManager::GetTransactionLocked(txn_id_t txn_id) {
    auto it = txn_map.find(txn_id);
    if (it == txn_map.end()) {
        return nullptr;
    }
    return it->second.Get();
}

void TransactionManager::RegisterTransactionLocked(std::unique_ptr<Transaction> txn) {
    txn_id_t txn_id = txn->get_transaction_id();
    txn_map[txn_id] = TxnEntry{std::move(txn), false};
}

void TransactionManager::SetTransactionRetainedLocked(txn_id_t txn_id, bool retained) {
    auto it = txn_map.find(txn_id);
    if (it == txn_map.end()) {
        return;
    }
    it->second.retained = retained;
}

void TransactionManager::RemoveTransactionLocked(txn_id_t txn_id,
                                                 std::vector<std::unique_ptr<Transaction>> *released_txns) {
    auto it = txn_map.find(txn_id);
    if (it == txn_map.end()) {
        return;
    }
    if (released_txns != nullptr && it->second.txn != nullptr) {
        released_txns->push_back(std::move(it->second.txn));
    }
    txn_map.erase(it);
}

//--------------------------------------------------------------------------
// begin  — start a new transaction
//--------------------------------------------------------------------------
/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 * @param {IsolationLevel} isolation_level 隔离级别
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager,
                                       IsolationLevel isolation_level) {
    auto write_guard = write_txn_guard();
    // 1. 创建新事务（忽略传入的 txn 参数，始终由 Manager 管理所有权）
    // 2. 把开始事务加入到全局事务表中
    // 3. 返回当前事务指针
    auto owned = std::make_unique<Transaction>(next_txn_id_++, isolation_level);
    txn = owned.get();
    txn->set_state(TransactionState::GROWING);
    txn->set_isolation_level(isolation_level);
    {
        std::lock_guard<std::mutex> wm_lock(watermark_mutex_);
        timestamp_t start_ts = last_commit_ts_.load(std::memory_order_acquire);
        txn->set_start_ts(start_ts);
        running_txns_.AddTxn(start_ts);
    }
    {
        std::unique_lock<std::shared_mutex> txn_lock(txn_map_mutex_);
        RegisterTransactionLocked(std::move(owned));
    }

    if (log_manager != nullptr) {
        BeginLogRecord log_record(txn->get_transaction_id());
        log_record.prev_lsn_ = txn->get_prev_lsn(); // record the previous lsn
        lsn_t lsn = log_manager->add_log_to_buffer(&log_record);
        // now update prev_lsn_ to this log's lsn
        txn->set_prev_lsn(lsn);
    }

    return txn;
}

//--------------------------------------------------------------------------
// commit  — commit a transaction
//--------------------------------------------------------------------------

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction *txn, LogManager *log_manager) {
    auto write_guard = write_txn_guard();
    // 1. 如果存在未提交的写操作，提交所有的写操作
    // 2. 释放所有锁
    // 3. 释放事务相关资源，eg.锁集
    // 4. 把事务日志刷入磁盘中
    // 5. 更新事务状态
    // 如果需要支持MVCC请在上述过程中添加代码
    if (txn == nullptr) {
        return;
    }
    txn_id_t txn_id = txn->get_transaction_id();
    timestamp_t commit_ts = INVALID_TS;

    if (log_manager != nullptr) {
        CommitLogRecord log_record(txn->get_transaction_id());
        log_record.prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = log_manager->add_log_to_buffer(&log_record);
        txn->set_prev_lsn(lsn);
        log_manager->flush_log_to_disk();
    }

    bool has_ssi_state = txn->get_isolation_level() == IsolationLevel::SERIALIZABLE &&
                         (!txn->GetRecordReadSet().empty() || !txn->GetPredicateReadSet().empty() ||
                          !txn->GetWriteSetRecords().empty() || !txn->GetRwInEdges().empty() ||
                          !txn->GetRwOutEdges().empty());
    bool retain_txn = IsMvccActive(txn) && (txn->GetUndoLogNum() > 0 || has_ssi_state);
    txn->set_state(TransactionState::SHRINKING);
    std::vector<std::unique_ptr<Transaction>> released_txns;

    // Commit timestamp publication precedes version-link finalization.
    commit_ts = next_timestamp_.fetch_add(1, std::memory_order_acq_rel) + 1;
    txn->set_commit_ts(commit_ts);
    txn->set_state(TransactionState::COMMITTED);
    last_commit_ts_.store(commit_ts, std::memory_order_release);

    // Version links are finalized per tuple without holding the transaction map lock.
    if (IsMvccActive(txn) && sm_manager_ != nullptr) {
        // For repeated writes on one tuple, the last write record carries the
        // committed tuple state represented by the current version-link head.
        auto &write_set = *txn->get_write_set();
        for (auto it = write_set.rbegin(); it != write_set.rend(); ++it) {
            auto *write_record = *it;
            const auto &tab_name = write_record->GetTableName();
            auto metadata_lock = sm_manager_->LockMetadataShared();
            auto fh_it = sm_manager_->fhs_.find(tab_name);
            if (fh_it == sm_manager_->fhs_.end()) {
                continue;
            }
            int table_fd = fh_it->second->GetFd();
            metadata_lock.unlock();
            CommitVersionLink(table_fd, write_record->GetRid(), txn_id, commit_ts,
                              write_record->GetWriteType() == WType::DELETE_TUPLE);
        }
    }

    // Reader watermark removal happens after committed versions are visible.
    {
        std::lock_guard<std::mutex> wm_lock(watermark_mutex_);
        running_txns_.UpdateCommitTs(commit_ts);
        running_txns_.RemoveTxn(txn->get_start_ts());
    }

    // Retained transactions keep undo logs and SSI edges until garbage collection.
    {
        std::unique_lock<std::shared_mutex> txn_lock(txn_map_mutex_);
        if (retain_txn) {
            SetTransactionRetainedLocked(txn_id, true);
        } else {
            RemoveTransactionLocked(txn_id, &released_txns);
        }
    }

    release_locks(txn, lock_manager_);
    if (!retain_txn) {
        delete_write_set(txn);
    }
    RequestGarbageCollection();
}

//--------------------------------------------------------------------------
// MVCC index cleanup helpers
//--------------------------------------------------------------------------

auto index_key_equal(const IndexMeta &index, const RmRecord &lhs, const RmRecord &rhs) -> bool {
    auto lhs_key = std::make_unique<char[]>(index.col_tot_len);
    auto rhs_key = std::make_unique<char[]>(index.col_tot_len);
    index.build_key(lhs_key.get(), lhs.data);
    index.build_key(rhs_key.get(), rhs.data);
    return memcmp(lhs_key.get(), rhs_key.get(), index.col_tot_len) == 0;
}

void delete_changed_mvcc_index_entries(SmManager *sm_manager, const std::string &tab_name, const TabMeta &tab,
                                       const RmRecord &old_record, const RmRecord &current_record, const Rid &rid,
                                       Transaction *txn) {
    for (auto &index : tab.indexes) {
        if (index_key_equal(index, old_record, current_record)) {
            continue;
        }
        auto ih = sm_manager->get_ih(tab_name, index.cols);
        auto key = std::make_unique<char[]>(index.col_tot_len);
        index.build_key(key.get(), current_record.data);
        ih->delete_entry(key.get(), rid, txn);
    }
}

/**
 * Deletes stale MVCC index entries for a record that has been updated or deleted, used during transaction abort to clean up index entries that are no longer visible.
 */
void delete_stale_mvcc_index_entries(SmManager *sm_manager, const std::string &tab_name, const TabMeta &tab,
                                     const RmRecord &stale_record, const RmRecord &current_record, const Rid &rid,
                                     Transaction *txn) {
    for (auto &index : tab.indexes) {
        if (index_key_equal(index, stale_record, current_record)) {
            continue;
        }
        auto ih = sm_manager->get_ih(tab_name, index.cols);
        auto key = std::make_unique<char[]>(index.col_tot_len);
        index.build_key(key.get(), stale_record.data);
        ih->delete_entry(key.get(), rid, txn);
    }
}

//--------------------------------------------------------------------------
// abort  — roll back a transaction
//--------------------------------------------------------------------------

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction *txn, LogManager *log_manager) {
    auto write_guard = write_txn_guard();
    // 1. 回滚所有写操作
    // 2. 释放所有锁
    // 3. 清空事务相关资源，eg.锁集
    // 4. 把事务日志刷入磁盘中
    // 5. 更新事务状态
    // 如果需要支持MVCC请在上述过程中添加代码
    if (txn == nullptr) {
        return;
    }
    txn_id_t txn_id = txn->get_transaction_id();
    txn->set_state(TransactionState::ABORTED);
    bool use_mvcc = IsMvccActive(txn);
    bool undo_failed = false;
    auto &write_set = *txn->get_write_set();
    for (auto it = write_set.rbegin(); it != write_set.rend(); ++it) {
        if (undo_failed) {
            break;
        }
        WriteRecord *write_record = *it;
        try {
            const std::string &tab_name = write_record->GetTableName();
            auto metadata_lock = sm_manager_->LockMetadataShared();
            auto fh_it = sm_manager_->fhs_.find(tab_name);
            if (fh_it == sm_manager_->fhs_.end()) {
                continue;
            }

            RmFileHandle *fh = fh_it->second.get();
            TabMeta &tab = sm_manager_->db_.get_table(tab_name);

            if (write_record->GetWriteType() == WType::INSERT_TUPLE) {
                auto rec = fh->get_record(write_record->GetRid(), nullptr);
                DeleteLogRecord clr(txn->get_transaction_id(), *rec, write_record->GetRid(), tab_name);
                append_txn_clr(txn, log_manager, clr, LogType::CLR_DELETE, write_record->GetOpPrevLsn());
                apply_index_op(sm_manager_, tab_name, tab, *rec, write_record->GetRid(), txn, IndexEntryOp::DELETE);
                fh->delete_record(write_record->GetRid(), nullptr, txn->get_prev_lsn());
            } else if (write_record->GetWriteType() == WType::DELETE_TUPLE) {
                InsertLogRecord clr(txn->get_transaction_id(), write_record->GetRecord(), write_record->GetRid(),
                                    tab_name);
                append_txn_clr(txn, log_manager, clr, LogType::CLR_INSERT, write_record->GetOpPrevLsn());
                if (fh->is_record(write_record->GetRid())) {
                    fh->update_record(write_record->GetRid(), write_record->GetRecord().data, nullptr,
                                      txn->get_prev_lsn());
                } else {
                    fh->insert_record(write_record->GetRid(), write_record->GetRecord().data, txn->get_prev_lsn());
                }
                if (!use_mvcc) {
                    apply_index_op(sm_manager_, tab_name, tab, write_record->GetRecord(), write_record->GetRid(), txn,
                                   IndexEntryOp::INSERT);
                }
            } else if (write_record->GetWriteType() == WType::UPDATE_TUPLE) {
                auto current_rec = fh->get_record(write_record->GetRid(), nullptr);
                UpdateLogRecord clr(txn->get_transaction_id(), *current_rec, write_record->GetRecord(),
                                    write_record->GetRid(), tab_name);
                append_txn_clr(txn, log_manager, clr, LogType::CLR_UPDATE, write_record->GetOpPrevLsn());
                if (use_mvcc) {
                    delete_changed_mvcc_index_entries(sm_manager_, tab_name, tab, write_record->GetRecord(),
                                                      *current_rec, write_record->GetRid(), txn);
                } else {
                    apply_index_op(sm_manager_, tab_name, tab, *current_rec, write_record->GetRid(), txn,
                                   IndexEntryOp::DELETE);
                    apply_index_op(sm_manager_, tab_name, tab, write_record->GetRecord(), write_record->GetRid(), txn,
                                   IndexEntryOp::INSERT);
                }
                fh->update_record(write_record->GetRid(), write_record->GetRecord().data, nullptr,
                                  txn->get_prev_lsn());
            }
        } catch (const std::exception &error) {
            UndoError undo_error(txn->get_transaction_id(), write_record->GetTableName(), write_record->GetRid(),
                                 error.what());
            std::cerr << undo_error.what() << std::endl;
            undo_failed = true;
            break;
        } catch (...) {
            UndoError undo_error(txn->get_transaction_id(), write_record->GetTableName(), write_record->GetRid(),
                                 "unknown exception");
            std::cerr << undo_error.what() << std::endl;
            undo_failed = true;
            break;
        }
    }

    if (log_manager != nullptr) {
        AbortLogRecord abort_log(txn->get_transaction_id());
        abort_log.prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = log_manager->add_log_to_buffer(&abort_log);
        txn->set_prev_lsn(lsn);
        log_manager->flush_log_to_disk();
    }

    txn->set_state(TransactionState::SHRINKING);
    CleanupTransaction(txn);
    release_locks(txn, lock_manager_);
    delete_write_set(txn);
    txn->set_state(TransactionState::ABORTED);

    std::vector<std::unique_ptr<Transaction>> released_txns;
    {
        std::lock_guard<std::mutex> wm_lock(watermark_mutex_);
        running_txns_.RemoveTxn(txn->get_start_ts());
    }
    {
        std::unique_lock<std::shared_mutex> txn_lock(txn_map_mutex_);
        RemoveTransactionLocked(txn_id, &released_txns);
    }
    RequestGarbageCollection();
}

lsn_t TransactionManager::create_static_checkpoint(LogManager *log_manager) {
    std::unique_lock<std::shared_mutex> checkpoint_lock(checkpoint_mutex_);
    if (log_manager == nullptr || sm_manager_ == nullptr) {
        return INVALID_LSN;
    }

    //  step1: scan all txn which not finished
    std::vector<std::pair<txn_id_t, lsn_t>> active_txns;
    {
        std::shared_lock<std::shared_mutex> txn_lock(txn_map_mutex_);
        active_txns.reserve(txn_map.size());
        for (auto &[txn_id, entry] : txn_map) {
            Transaction *txn = entry.Get();
            if (txn == nullptr) {
                continue;
            }
            TransactionState state = txn->get_state();
            if (state == TransactionState::COMMITTED || state == TransactionState::ABORTED) {
                continue;
            }
            active_txns.emplace_back(txn_id, txn->get_prev_lsn());
        }
    }

    // step 2: record checkpoint
    CheckpointLogRecord checkpoint(active_txns);
    LogAppendResult checkpoint_append = log_manager->add_log_to_buffer_with_result(&checkpoint);
    if (!checkpoint_append.IsValid()) {
        throw InternalError("checkpoint log record is too large");
    }
    log_manager->flush_log_to_disk();

    RunCheckpointGarbageCollection();

    if (!sm_manager_->flush_storage()) {
        throw InternalError("checkpoint storage flush failed");
    }

    write_restart_file(RestartRecord{checkpoint_append.lsn, checkpoint_append.offset});

    return checkpoint_append.lsn;
}

//--------------------------------------------------------------------------
// Version Link Management — MVCC version chain accessors
//--------------------------------------------------------------------------

bool TransactionManager::UpdateVersionLink(int fd, Rid rid, std::optional<VersionUndoLink> prev_version,
                                           std::function<bool(std::optional<VersionUndoLink>)> &&check) {
    VersionPageKey key{fd, rid.page_no};
    std::shared_ptr<PageVersionInfo> page_info;
    {
        std::unique_lock<std::shared_mutex> table_lock(version_info_mutex_);
        auto &slot = version_info_[key];
        if (slot == nullptr) {
            slot = std::make_shared<PageVersionInfo>();
        }
        page_info = slot;
    }

    std::unique_lock<std::shared_mutex> page_lock(page_info->mutex_);
    std::optional<VersionUndoLink> current = std::nullopt;
    auto it = page_info->prev_version_.find(rid.slot_no);
    if (it != page_info->prev_version_.end()) {
        current = it->second;
    }
    if (check != nullptr && !check(current)) {
        return false;
    }
    if (prev_version.has_value()) {
        page_info->prev_version_[rid.slot_no] = *prev_version;
    } else {
        page_info->prev_version_.erase(rid.slot_no);
    }
    return true;
}

std::optional<VersionUndoLink> TransactionManager::GetVersionLink(int fd, Rid rid) {
    VersionPageKey key{fd, rid.page_no};
    std::shared_lock<std::shared_mutex> table_lock(version_info_mutex_);
    auto page_it = version_info_.find(key);
    if (page_it == version_info_.end()) {
        return std::nullopt;
    }
    auto page_info = page_it->second;
    table_lock.unlock();

    std::shared_lock<std::shared_mutex> page_lock(page_info->mutex_);
    auto slot_it = page_info->prev_version_.find(rid.slot_no);
    if (slot_it == page_info->prev_version_.end()) {
        return std::nullopt;
    }
    return slot_it->second;
}

//--------------------------------------------------------------------------
// Undo Log Access
//--------------------------------------------------------------------------

std::optional<UndoLog> TransactionManager::GetUndoLogOptional(UndoLink link) {
    if (!link.IsValid()) {
        return std::nullopt;
    }
    std::shared_lock<std::shared_mutex> lock(txn_map_mutex_);
    auto *txn = GetTransactionLocked(link.prev_txn_);
    if (txn == nullptr) {
        return std::nullopt;
    }
    if (static_cast<size_t>(link.prev_log_idx_) >= txn->GetUndoLogNum()) {
        return std::nullopt;
    }
    return txn->GetUndoLog(link.prev_log_idx_);
}

UndoLog TransactionManager::GetUndoLog(UndoLink link) {
    auto log = GetUndoLogOptional(link);
    if (!log.has_value()) {
        throw InternalError("Undo log not found");
    }
    return *log;
}

//--------------------------------------------------------------------------
// MVCC Visibility & Conflict Detection
//--------------------------------------------------------------------------

//--------------------------------------------------------------------------
// GetVisibleRecord  — resolve MVCC snapshot visibility
//--------------------------------------------------------------------------
auto TransactionManager::GetVisibleRecord(int fd, const Rid &rid, const RmRecord &base_record, Transaction *txn)
    -> std::unique_ptr<RmRecord> {
    if (!IsMvccActive(txn)) {
        return std::make_unique<RmRecord>(base_record);
    }

    auto visible_from_log = [&](const UndoLog &log) -> bool {
        if (is_uncommitted_ts(log.ts_)) {
            return txn_id_from_write_ts(log.ts_) == txn->get_transaction_id();
        }
        return log.ts_ != INVALID_TS && log.ts_ <= txn->get_start_ts();
    };

    auto link = GetVersionLink(fd, rid);
    if (!link.has_value()) {
        return std::make_unique<RmRecord>(base_record);
    }

    if (link->in_progress_ && link->prev_.prev_txn_ == txn->get_transaction_id()) {
        if (link->is_deleted_) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(base_record);
    }

    if (!link->in_progress_ && link->ts_ != INVALID_TS && link->ts_ <= txn->get_start_ts()) {
        if (link->is_deleted_) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(base_record);
    }

    // Re-read when the version link is in_progress but owned by another
    // transaction.  The owner may have been mid-commit when we first looked;
    // re-reading under the page mutex picks up the committed state so we can
    // resolve visibility through the version link directly instead of falling
    // through to a costly (and possibly doomed) undo-chain walk.
    if (link->in_progress_ && link->prev_.prev_txn_ != txn->get_transaction_id()) {
        link = GetVersionLink(fd, rid);
        if (!link.has_value()) {
            return std::make_unique<RmRecord>(base_record);
        }
        if (link->in_progress_ && link->prev_.prev_txn_ == txn->get_transaction_id()) {
            if (link->is_deleted_) return nullptr;
            return std::make_unique<RmRecord>(base_record);
        }
        if (!link->in_progress_ && link->ts_ != INVALID_TS && link->ts_ <= txn->get_start_ts()) {
            if (link->is_deleted_) return nullptr;
            return std::make_unique<RmRecord>(base_record);
        }
        // Still can't resolve — fall through to chain walk below with the
        // latest snapshot of the version link.
    }

    UndoLink undo_link = link->prev_;
    while (undo_link.IsValid()) {
        auto undo_log = GetUndoLogOptional(undo_link);
        if (!undo_log.has_value()) {
            break;
        }
        if (visible_from_log(*undo_log)) {
            if (undo_log->is_deleted_) {
                return nullptr;
            }
            return std::make_unique<RmRecord>(*undo_log->old_record_);
        }
        undo_link = undo_log->prev_version_;
    }

    return nullptr;
}

//--------------------------------------------------------------------------
// CheckWriteConflict  — detect write-write conflicts
//--------------------------------------------------------------------------
void TransactionManager::CheckWriteConflict(int fd, const Rid &rid, Transaction *txn) {
    if (!IsMvccActive(txn)) {
        return;
    }
    auto current = GetVersionLink(fd, rid);
    if (!current.has_value()) {
        return;
    }
    if (current->prev_.prev_txn_ == txn->get_transaction_id()) {
        return;
    }
    // 其他事务在修改这行且尚未提交
    if (current->in_progress_) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::WRITE_CONFLICT);
    }
    // 其他事务还是后提交，基于过期快照
    if (current->ts_ != INVALID_TS && current->ts_ > txn->get_start_ts()) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::WRITE_CONFLICT);
    }
}

void TransactionManager::TrackReadTable(int fd, Transaction *txn) {
    if (!IsMvccActive(txn)) {
        return;
    }
    txn->AddReadTableFd(fd);
}

//--------------------------------------------------------------------------
// Serializable Snapshot Isolation (SSI) — Conflict Detection
//--------------------------------------------------------------------------

//--------------------------------------------------------------------------
// TrackSsiPredicateRead  — register and check predicate-based reads
//--------------------------------------------------------------------------
void TransactionManager::TrackSsiPredicateRead(const std::string &table_name,
                                               const std::vector<Condition> &conditions, Transaction *txn) {
    if (!IsSerializableActive(txn)) {
        return;
    }
    txn->AddPredicateRead(table_name, conditions);

    TabMeta tab;
    {
        auto metadata_lock = sm_manager_->LockMetadataShared();
        tab = sm_manager_->db_.get_table(table_name);
    }
    {
        std::shared_lock<std::shared_mutex> lock(txn_map_mutex_);
        ForEachTransactionLocked([&](Transaction *writer) {
            if (!IsSerializableActive(writer) || writer->get_transaction_id() == txn->get_transaction_id()) {
                return;
            }
            if (ssi_writer_visible_to(writer, txn)) {
                return;
            }
            auto write_records = writer->GetWriteSetRecords();
            for (const auto &write : write_records) {
                if (write.table_name_ == table_name && ssi_write_matches_conditions(tab, write, conditions)) {
                    AddRwDependency(txn, writer, txn, "serializable predicate read sees older snapshot");
                    break;
                }
            }
        });
    }
}

//--------------------------------------------------------------------------
// TrackSsiRecordRead  — register and check record-level reads
//--------------------------------------------------------------------------
void TransactionManager::TrackSsiRecordRead(const std::string &table_name, const std::vector<Condition> &conditions,
                                            const Rid &rid, const RmRecord &record, Transaction *txn) {
    if (!IsSerializableActive(txn)) {
        return;
    }
    
    txn->AddRecordRead(table_name, rid);
    TabMeta tab;
    {
        auto metadata_lock = sm_manager_->LockMetadataShared();
        tab = sm_manager_->db_.get_table(table_name);
    }

    {
        std::shared_lock<std::shared_mutex> lock(txn_map_mutex_);
        // 遍历其他事务检查有无写者改动
        ForEachTransactionLocked([&](Transaction *writer) {
            if (!IsSerializableActive(writer) || writer->get_transaction_id() == txn->get_transaction_id()) {
                return;
            }
            if (ssi_writer_visible_to(writer, txn)) {
                return;
            }
            // 检查写集
            auto write_records = writer->GetWriteSetRecords();
            for (const auto &write : write_records) {
                if (write.table_name_ != table_name) {
                    continue;
                }
                if (write.rid_ == rid || ssi_write_matches_conditions(tab, write, conditions)) {
                    AddRwDependency(txn, writer, txn, "serializable read sees older snapshot");
                    break;
                }
            }
        });
    }
}

//--------------------------------------------------------------------------
// TrackSsiWrite  — register writes and detect rw-conflicts
//--------------------------------------------------------------------------
void TransactionManager::TrackSsiWrite(const std::string &table_name, const Rid &rid, const RmRecord *old_record,
                                       const RmRecord *new_record, Transaction *txn) {
    if (!IsSerializableActive(txn)) {
        return;
    }

    const RmRecord *stored_old = old_record != nullptr ? old_record : new_record;
    const RmRecord *stored_new = new_record != nullptr ? new_record : old_record;
    if (stored_old == nullptr || stored_new == nullptr) {
        return;
    }
    txn->AddSsiWriteSetRecord(table_name, rid, *stored_old, *stored_new, old_record != nullptr, new_record != nullptr);

    TabMeta tab;
    {
        auto metadata_lock = sm_manager_->LockMetadataShared();
        tab = sm_manager_->db_.get_table(table_name);
    }
    {
        std::shared_lock<std::shared_mutex> lock(txn_map_mutex_);
        ForEachTransactionLocked([&](Transaction *reader) {
            if (!IsSerializableActive(reader) || reader->get_transaction_id() == txn->get_transaction_id()) {
                return;
            }
            if (ssi_writer_visible_to(txn, reader)) {
                return;
            }

            auto record_reads = reader->GetRecordReadSet();
            bool matches = std::any_of(record_reads.begin(), record_reads.end(), [&](const SsiRecordRead &read) {
                return read.table_name_ == table_name && read.rid_ == rid;
            });

            if (!matches) {
                auto predicate_reads = reader->GetPredicateReadSet();
                for (const auto &predicate : predicate_reads) {
                    if (predicate.table_name_ != table_name) {
                        continue;
                    }
                    bool old_matches = old_record != nullptr &&
                                       evaluate_conditions(predicate.conditions_, *old_record, tab.cols);
                    bool new_matches = new_record != nullptr &&
                                       evaluate_conditions(predicate.conditions_, *new_record, tab.cols);
                    if (old_matches || new_matches) {
                        matches = true;
                        break;
                    }
                }
            }

            if (matches) {
                AddRwDependency(reader, txn, txn, "serializable write changes prior read");
            }
        });
    }
}

//--------------------------------------------------------------------------
// AddRwDependency  — add rw-edge and check for dangerous structures
//--------------------------------------------------------------------------
bool TransactionManager::AddRwDependency(Transaction *from, Transaction *to, Transaction *current,
                                         const std::string &reason) {
    if (!IsSerializableActive(from) || !IsSerializableActive(to) ||
        from->get_transaction_id() == to->get_transaction_id()) {
        return false;
    }

    from->AddRwDependencyOut(to->get_transaction_id(), reason);
    to->AddRwDependencyIn(from->get_transaction_id(), reason);

    // Always check for dangerous structures, even when the same edge was
    // previously added — transaction state may have changed since the first
    // check, creating a new pivot structure that needs detection.
    if (HasDangerousStructure(from, to)) {
        throw TransactionAbortException(current->get_transaction_id(), AbortReason::SERIALIZATION_CONFLICT);
    }
    return true;
}

//--------------------------------------------------------------------------
// HasDangerousStructure  — detect dangerous pivot structures: if a txn has 
//   a read-write dependency path that crosses a pivot transaction, it may
//   be unsafe to commit.
//--------------------------------------------------------------------------
bool TransactionManager::HasDangerousStructure(Transaction *from, Transaction *to) {
    if (from == nullptr || to == nullptr) {
        return false;
    }
    std::unordered_set<txn_id_t> visited;
    return has_rw_path_to(to, from->get_transaction_id(), visited);
}

bool TransactionManager::has_rw_path_to(Transaction *current, txn_id_t target,
                                         std::unordered_set<txn_id_t> &visited) {
    if (current == nullptr || !visited.insert(current->get_transaction_id()).second) {
        return false;
    }
    auto out_edges = current->GetRwOutEdges();
    for (const auto &[next_id, _] : out_edges) {
        if (next_id == target) {
            return true;
        }
        auto it = txn_map.find(next_id);
        if (it != txn_map.end() && has_rw_path_to(it->second.Get(), target, visited)) {
            return true;
        }
    }
    return false;
}

//--------------------------------------------------------------------------
// MVCC Write Preparation — build undo logs and link version chains
//--------------------------------------------------------------------------

//--------------------------------------------------------------------------
// PrepareInsert  — set up version link for INSERT
//--------------------------------------------------------------------------
void TransactionManager::PrepareInsert(int fd, const Rid &rid, const RmRecord &new_record, Transaction *txn) {
    if (!IsMvccActive(txn)) {
        return;
    }
    auto current = GetVersionLink(fd, rid);
    if (current.has_value()) {
        // Reusing a previously-deleted slot: build an undo log that preserves
        // the old version chain so MVCC readers can still traverse it.
        UndoLog log = BuildUndoLogForWrite(fd, rid, new_record, txn);
        UndoLink undo_link = txn->AppendUndoLog(std::move(log));
        UpdateVersionLink(fd, rid, VersionUndoLink{undo_link, true, make_txn_write_ts(txn), false});
    } else {
        // Fresh slot with no prior version chain.
        UpdateVersionLink(fd, rid, VersionUndoLink{UndoLink{txn->get_transaction_id(), -1}, true,
                                                   make_txn_write_ts(txn), false});
    }
}

//--------------------------------------------------------------------------
// BuildUndoLogForWrite  — construct undo log for UPDATE/DELETE
//--------------------------------------------------------------------------
UndoLog TransactionManager::BuildUndoLogForWrite(int fd, const Rid &rid, const RmRecord &base_record,
                                                  Transaction *txn) {
    UndoLog log;
    log.is_deleted_ = false;  // default: the pre-image was a live tuple
    log.old_record_ = std::make_unique<RmRecord>(base_record);
    auto current = GetVersionLink(fd, rid);
    if (current.has_value()) {
        if (current->prev_.prev_txn_ == txn->get_transaction_id()) {
            // Same transaction already owns this slot: inherit the earlier undo log so
            // the chain points to the state before the first write of this transaction.
            auto previous_log = GetUndoLog(current->prev_);
            log.is_deleted_ = previous_log.is_deleted_;
            log.old_record_ = std::make_unique<RmRecord>(*previous_log.old_record_);
            log.ts_ = previous_log.ts_;
            log.prev_version_ = previous_log.prev_version_;
        } else {
            log.ts_ = current->ts_;
            log.prev_version_ = current->prev_;
        }
    } else {
        log.ts_ = 0;
    }
    return log;
}

//--------------------------------------------------------------------------
// PrepareUpdate / PrepareDelete  — set up version links
//--------------------------------------------------------------------------
void TransactionManager::PrepareUpdate(int fd, const Rid &rid, const RmRecord &old_record, Transaction *txn) {
    if (!IsMvccActive(txn)) {
        return;
    }
    // build undo log
    UndoLog log = BuildUndoLogForWrite(fd, rid, old_record, txn);
    UndoLink undo_link = txn->AppendUndoLog(std::move(log));
    UpdateVersionLink(fd, rid, VersionUndoLink{undo_link, true, make_txn_write_ts(txn), false});
}

void TransactionManager::PrepareDelete(int fd, const Rid &rid, const RmRecord &old_record, Transaction *txn) {
    if (!IsMvccActive(txn)) {
        return;
    }
    UndoLog log = BuildUndoLogForWrite(fd, rid, old_record, txn);
    UndoLink undo_link = txn->AppendUndoLog(std::move(log));
    UpdateVersionLink(fd, rid, VersionUndoLink{undo_link, true, make_txn_write_ts(txn), true});
}

//--------------------------------------------------------------------------
// Version Link Finalization
//--------------------------------------------------------------------------

//--------------------------------------------------------------------------
// CommitVersionLink  — finalize in-progress version link on commit
//--------------------------------------------------------------------------
void TransactionManager::CommitVersionLink(int fd, const Rid &rid, txn_id_t txn_id,
                                           timestamp_t commit_ts, bool is_delete) {
    VersionPageKey key{fd, rid.page_no};
    std::shared_ptr<PageVersionInfo> page_info;
    {
        std::unique_lock<std::shared_mutex> table_lock(version_info_mutex_);
        auto it = version_info_.find(key);
        if (it == version_info_.end()) {
            return;
        }
        page_info = it->second;
    }
    {
        std::unique_lock<std::shared_mutex> page_lock(page_info->mutex_);
        auto slot_it = page_info->prev_version_.find(rid.slot_no);
        if (slot_it == page_info->prev_version_.end()) {
            return;
        }
        auto &link = slot_it->second;
        if (link.prev_.prev_txn_ != txn_id) {
            return;
        }
        if (!link.in_progress_) {
            return;
        }
        // commited
        link.in_progress_ = false;
        link.ts_ = commit_ts;
        link.is_deleted_ = is_delete;
    }
}

//--------------------------------------------------------------------------
// ClearVersionLink  — remove version link entry
//--------------------------------------------------------------------------
void TransactionManager::ClearVersionLink(int fd, const Rid &rid) {
    UpdateVersionLink(fd, rid, std::nullopt);
}

//--------------------------------------------------------------------------
// Cleanup & Garbage Collection
//--------------------------------------------------------------------------

//--------------------------------------------------------------------------
// RemoveSsiEdgesForTxnLocked  — scrub all rw-edges for a txn
//--------------------------------------------------------------------------

void TransactionManager::RemoveSsiEdgesForTxnLocked(txn_id_t txn_id) {
    ForEachTransactionLocked([&](Transaction *other) {
        if (other == nullptr || other->get_transaction_id() == txn_id) {
            return;
        }
        other->RemoveRwDependencyIn(txn_id);
        other->RemoveRwDependencyOut(txn_id);
    });
}

//--------------------------------------------------------------------------
// CleanupTransaction  — revert version links and clear SSI state
//--------------------------------------------------------------------------

void TransactionManager::CleanupTransaction(Transaction *txn) {
    if (txn == nullptr) {
        return;
    }
    for (auto *write_record : *txn->get_write_set()) {
        const auto &tab_name = write_record->GetTableName();
        auto metadata_lock = sm_manager_->LockMetadataShared();
        auto fh_it = sm_manager_->fhs_.find(tab_name);
        if (fh_it == sm_manager_->fhs_.end()) {
            continue;
        }
        int table_fd = fh_it->second->GetFd();
        metadata_lock.unlock();
        auto link = GetVersionLink(table_fd, write_record->GetRid());
        if (link.has_value() && link->prev_.prev_txn_ == txn->get_transaction_id()) {
            if (!link->prev_.IsValid()) {
                UpdateVersionLink(table_fd, write_record->GetRid(), std::nullopt);
                continue;
            }
            auto undo_log = txn->GetUndoLog(link->prev_.prev_log_idx_);
            std::optional<VersionUndoLink> restored = std::nullopt;
            if (undo_log.ts_ != make_txn_write_ts(txn) && undo_log.ts_ != 0) {
                restored = VersionUndoLink{undo_log.prev_version_, false, undo_log.ts_, undo_log.is_deleted_};
            }
            UpdateVersionLink(table_fd, write_record->GetRid(), restored);
        }
    }
    {
        std::shared_lock<std::shared_mutex> lock(txn_map_mutex_);
        RemoveSsiEdgesForTxnLocked(txn->get_transaction_id());
    }
    txn->ClearSsiState();
}

//--------------------------------------------------------------------------
// GetWatermark  — return oldest active snapshot timestamp
//--------------------------------------------------------------------------

timestamp_t TransactionManager::GetWatermark() {
    std::lock_guard<std::mutex> lock(watermark_mutex_);
    return running_txns_.GetWatermark();
}

//--------------------------------------------------------------------------
// GarbageCollection  — reclaim version info and physical space
//--------------------------------------------------------------------------
void TransactionManager::GarbageCollection() {
    RunGarbageCollection(false, false);
}

void TransactionManager::RunCheckpointGarbageCollection() {
    RunGarbageCollection(false, true);
}

void TransactionManager::RunGarbageCollection(bool apply_throttle) {
    RunGarbageCollection(apply_throttle, false);
}

void TransactionManager::RunGarbageCollection(bool apply_throttle, bool force_reclaim_committed_versions) {
    std::shared_lock<std::shared_mutex> checkpoint_guard;
    if (!force_reclaim_committed_versions) {
        checkpoint_guard = std::shared_lock<std::shared_mutex>(checkpoint_mutex_);
    }

    std::vector<GcDeleteRecord> deleted_records;
    std::vector<GcVersionRecord> reclaimed_versions;
    std::vector<std::unique_ptr<Transaction>> released_txns;
    gc_counters_.attempts.fetch_add(1, std::memory_order_acq_rel);

    timestamp_t watermark = INVALID_TS;
    bool has_active_readers = false;
    auto refresh_reader_snapshot = [&] {
        watermark = running_txns_.GetWatermark();
        has_active_readers = !running_txns_.current_reads_.empty();
    };

    // Committed version heads are detached under version-info locks.
    {
        std::lock_guard<std::mutex> wm_lock(watermark_mutex_);
        refresh_reader_snapshot();
        if (!force_reclaim_committed_versions && watermark == INVALID_TS) {
            return;
        }
        std::unique_lock<std::shared_mutex> table_lock(version_info_mutex_);
        for (auto page_it = version_info_.begin(); page_it != version_info_.end();) {
            auto page_info = page_it->second;
            {
                std::unique_lock<std::shared_mutex> page_lock(page_info->mutex_);
                for (auto slot_it = page_info->prev_version_.begin(); slot_it != page_info->prev_version_.end();) {
                    const auto &version = slot_it->second;
                    bool reclaimable = force_reclaim_committed_versions
                                           ? checkpoint_version_safe_to_reclaim(version)
                                           : gc_version_safe_to_reclaim(version, watermark, has_active_readers);
                    if (!reclaimable) {
                        ++slot_it;
                        continue;
                    }
                    Rid rid{page_it->first.page_no, static_cast<int>(slot_it->first)};
                    reclaimed_versions.push_back(GcVersionRecord{page_it->first.fd, rid, version});
                    slot_it = page_info->prev_version_.erase(slot_it);
                }
            }
            if (page_info->prev_version_.empty()) {
                page_it = version_info_.erase(page_it);
            } else {
                ++page_it;
            }
        }
    }
    gc_counters_.reclaimed_versions.fetch_add(reclaimed_versions.size(), std::memory_order_acq_rel);

    uint64_t work_budget = 0;
    auto maybe_throttle = [&] {
        if (!apply_throttle || kGarbageCollectionWorkBudget == 0 || work_budget < kGarbageCollectionWorkBudget) {
            return;
        }
        work_budget = 0;
        gc_counters_.throttle_waits.fetch_add(1, std::memory_order_acq_rel);
        std::this_thread::sleep_for(std::chrono::milliseconds(kGarbageCollectionThrottleMs));
    };

    for (size_t i = 0; i < reclaimed_versions.size(); ++i) {
        ++work_budget;
        maybe_throttle();
    }

    // Undo records identify obsolete index keys that no active snapshot can require.
    std::vector<GcStaleIndexRecord> stale_index_records;
    for (const auto &entry : reclaimed_versions) {
        std::unique_ptr<RmRecord> current_record;
        {
            if (sm_manager_ == nullptr) {
                continue;
            }
            auto metadata_lock = sm_manager_->LockMetadataShared();
            auto [tab_name, fh] = find_table_by_fd_unlocked(sm_manager_, entry.fd);
            if (fh == nullptr) {
                continue;
            }

            try {
                current_record = fh->get_record_no_mvcc(entry.rid);
            } catch (const RecordNotFoundError &) {
            }
        }

        if (entry.version.is_deleted_) {
            deleted_records.push_back(GcDeleteRecord{entry.fd, entry.rid});
            if (current_record != nullptr) {
                stale_index_records.push_back(
                    GcStaleIndexRecord{entry.fd, entry.rid, *current_record, true});
            }
        }

        UndoLink undo_link = entry.version.prev_;
        while (undo_link.IsValid()) {
            auto undo_log = GetUndoLogOptional(undo_link);
            if (!undo_log.has_value()) {
                break;
            }
            if (!undo_log->is_deleted_ && undo_log->old_record_ != nullptr && current_record != nullptr) {
                stale_index_records.push_back(
                    GcStaleIndexRecord{entry.fd, entry.rid, *undo_log->old_record_, false});
            }
            undo_link = undo_log->prev_version_;
        }
    }

    // Obsolete index entries are removed before any heap slot is physically freed.
    for (const auto &entry : stale_index_records) {
        if (sm_manager_ == nullptr) {
            continue;
        }
        auto metadata_lock = sm_manager_->LockMetadataShared();
        auto [tab_name, fh] = find_table_by_fd_unlocked(sm_manager_, entry.fd);
        if (fh == nullptr) {
            continue;
        }
        try {
            auto &tab = sm_manager_->db_.get_table(tab_name);
            if (entry.row_deleted) {
                apply_index_op(sm_manager_, tab_name, tab, entry.record, entry.rid, nullptr, IndexEntryOp::DELETE);
                gc_counters_.stale_index_deletes.fetch_add(1, std::memory_order_acq_rel);
                ++work_budget;
                maybe_throttle();
                continue;
            }
            auto current_record = fh->get_record_no_mvcc(entry.rid);
            delete_stale_mvcc_index_entries(sm_manager_, tab_name, tab, entry.record, *current_record,
                                            entry.rid, nullptr);
            gc_counters_.stale_index_deletes.fetch_add(1, std::memory_order_acq_rel);
            ++work_budget;
            maybe_throttle();
        } catch (const RecordNotFoundError &) {
        }
    }

    // Logical deletes become physical deletes only after index cleanup.
    for (const auto &entry : deleted_records) {
        if (sm_manager_ == nullptr) {
            continue;
        }
        auto metadata_lock = sm_manager_->LockMetadataShared();
        auto [_, fh] = find_table_by_fd_unlocked(sm_manager_, entry.fd);
        if (fh == nullptr) {
            continue;
        }
        try {
            fh->delete_record(entry.rid, nullptr);
            gc_counters_.physical_deletes.fetch_add(1, std::memory_order_acq_rel);
            ++work_budget;
            maybe_throttle();
        } catch (const RecordNotFoundError &) {
        }
    }

    // Retained transactions can be released once their undo chains are unreachable.
    std::vector<VersionUndoLink> remaining_versions;
    std::unordered_set<txn_id_t> referenced_txns;
    {
        std::shared_lock<std::shared_mutex> table_lock(version_info_mutex_);
        for (const auto &[_, page_info] : version_info_) {
            std::shared_lock<std::shared_mutex> page_lock(page_info->mutex_);
            for (const auto &[_, version] : page_info->prev_version_) {
                remaining_versions.push_back(version);
            }
        }
    }
    for (const auto &version : remaining_versions) {
        collect_referenced_txns_from_chain(version, &referenced_txns, this);
    }
    {
        std::unique_lock<std::shared_mutex> txn_write_lock(txn_map_mutex_);
        std::vector<txn_id_t> retained_ids;
        for (auto &[txn_id, entry] : txn_map) {
            auto *txn = entry.Get();
            bool active = txn != nullptr && txn->get_state() != TransactionState::COMMITTED &&
                          txn->get_state() != TransactionState::ABORTED;
            if (entry.retained && !active && referenced_txns.find(txn_id) == referenced_txns.end()) {
                retained_ids.push_back(txn_id);
            }
        }
        for (txn_id_t txn_id : retained_ids) {
            RemoveSsiEdgesForTxnLocked(txn_id);
            auto it = txn_map.find(txn_id);
            if (it != txn_map.end()) {
                auto *t = it->second.Get();
                if (t != nullptr) {
                    delete_write_set(t);
                }
            }
            RemoveTransactionLocked(txn_id, &released_txns);
            gc_counters_.retained_txn_releases.fetch_add(1, std::memory_order_acq_rel);
        }
    }
}
