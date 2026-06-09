/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "transaction.h"
#include "watermark.h"
#include "recovery/log_manager.h"
#include "concurrency/lock_manager.h"
#include "system/sm_manager.h"
#include "common/exception.h"

/// 版本链头节点：将堆元组与其第一条 undo log 相连。
///
/// 状态机：
///   in_progress_ == true
///     该版本由一个未提交事务持有。ts_ = make_txn_write_ts(txn)（临时写时间戳，
///     值 >= TXN_START_ID，不可与 commit_ts 比较）。
///     仅 Prepare{Insert,Update,Delete} 写入此状态。
///
///   in_progress_ == false
///     该版本已提交。ts_ 为实际 commit_ts（>= 最近 watermark）。
///     仅 commit / CleanupTransaction（abort 回滚时）写入此状态。
///
///   is_deleted_ == true
///     堆元组在此版本时刻已被逻辑删除，对 ts_ 之后开始的事务不可见。
///     GC 负责在所有活跃事务都能看到此版本后物理删除该 slot。
///
/// 不变量：
///   in_progress_ 从 true 翻转为 false 只发生在 commit 路径；
///   abort 路径通过 CleanupTransaction 直接将 VersionUndoLink 恢复到
///   上一个已提交状态（或清除），不经过 in_progress_=false 的中间态。
struct VersionUndoLink {
    UndoLink prev_;
    bool in_progress_{false};   // true = 未提交；ts_ 为临时写时间戳
    timestamp_t ts_{INVALID_TS}; // 未提交时为 make_txn_write_ts 值，提交后为 commit_ts
    bool is_deleted_{false};    // 此版本是否为逻辑删除

    friend auto operator==(const VersionUndoLink &a, const VersionUndoLink &b) {
        return a.prev_ == b.prev_ && a.in_progress_ == b.in_progress_ && a.ts_ == b.ts_ &&
               a.is_deleted_ == b.is_deleted_;
    }

    friend auto operator!=(const VersionUndoLink &a, const VersionUndoLink &b) { return !(a == b); }

    inline static std::optional<VersionUndoLink> FromOptionalUndoLink(std::optional<UndoLink> undo_link) {
        if (undo_link.has_value()) {
            return VersionUndoLink{*undo_link};
        }
        return std::nullopt;
    }
};

class TransactionManager: public NonCopyable {
public:
    explicit TransactionManager(LockManager *lock_manager, SmManager *sm_manager) {
        sm_manager_ = sm_manager;
        lock_manager_ = lock_manager;
        StartGarbageCollectionWorker();
    }

    ~TransactionManager();

    Transaction* begin(Transaction* txn, LogManager* log_manager,
                       IsolationLevel isolation_level = IsolationLevel::SNAPSHOT_ISOLATION);

    void commit(Transaction* txn, LogManager* log_manager);

    void abort(Transaction* txn, LogManager* log_manager);

    lsn_t create_static_checkpoint(LogManager *log_manager);

    LockManager* get_lock_manager() { return lock_manager_; }

    static bool IsMvccActive(Transaction *txn) {
        return txn != nullptr && (txn->get_isolation_level() == IsolationLevel::SNAPSHOT_ISOLATION ||
                                  txn->get_isolation_level() == IsolationLevel::SERIALIZABLE);
    }

    static bool IsSerializableActive(Transaction *txn) {
        return txn != nullptr && txn->get_isolation_level() == IsolationLevel::SERIALIZABLE &&
               txn->get_state() != TransactionState::ABORTED;
    }

    /**
     * @description: 获取事务ID为txn_id的事务对象
     * @return {Transaction*} 事务对象的指针
     * @param {txn_id_t} txn_id 事务ID
     */
    Transaction* get_transaction(txn_id_t txn_id) {
        if(txn_id == INVALID_TXN_ID) return nullptr;

        std::shared_lock<std::shared_mutex> lock(txn_map_mutex_);
        auto *res = GetTransactionLocked(txn_id);
        if (res == nullptr) {
            return nullptr;
        }
        lock.unlock();
        assert(res != nullptr);
        assert(res->get_thread_id() == std::this_thread::get_id());

        return res;
    }

    /** ------------------------以下函数仅可能在MVCC当中使用------------------------------------------*/

    /** @brief 访问事务撤销日志缓冲区并获取撤销日志。如果事务不存在，返回 nullopt。
     * 如果索引超出范围仍然会抛出异常。 */
    std::optional<UndoLog> GetUndoLogOptional(UndoLink link);

    /** @brief 访问事务撤销日志缓冲区并获取撤销日志。除非访问当前事务缓冲区，
     * 否则应该始终调用此函数以获取撤销日志，而不是手动检索事务 shared_ptr 并访问缓冲区。 */
    UndoLog GetUndoLog(UndoLink link);

    /** @brief 获取系统中的最低读时间戳。 */
    timestamp_t GetWatermark();

    auto GetVisibleRecord(int fd, const Rid &rid, const RmRecord &base_record, Transaction *txn)
        -> std::unique_ptr<RmRecord>;

    void PrepareInsert(int fd, const Rid &rid, const RmRecord &new_record, Transaction *txn);

    void PrepareUpdate(int fd, const Rid &rid, const RmRecord &old_record, Transaction *txn);

    void PrepareDelete(int fd, const Rid &rid, const RmRecord &old_record, Transaction *txn);

    /**
     * @brief Atomically transition a version link from in_progress to committed.
     *
     * Reads the current version link, verifies that the owning transaction matches
     * `txn_id`, and updates the link to the committed state — all under the page
     * mutex.  This avoids a TOCTOU race between a separate GetVersionLink /
     * UpdateVersionLink pair.
     */
    void CommitVersionLink(int fd, const Rid &rid, txn_id_t txn_id, timestamp_t commit_ts, bool is_delete);

    void CheckWriteConflict(int fd, const Rid &rid, Transaction *txn);

    void TrackReadTable(int fd, Transaction *txn);

    void TrackSsiPredicateRead(const std::string &table_name, const std::vector<Condition> &conditions,
                               Transaction *txn);

    void TrackSsiRecordRead(const std::string &table_name, const std::vector<Condition> &conditions, const Rid &rid,
                            const RmRecord &record, Transaction *txn);

    void TrackSsiWrite(const std::string &table_name, const Rid &rid, const RmRecord *old_record,
                       const RmRecord *new_record, Transaction *txn);

    void ClearVersionLink(int fd, const Rid &rid);

    struct GarbageCollectionStats {
        uint64_t requests{0};
        uint64_t attempts{0};
        uint64_t worker_wakeups{0};
        uint64_t reclaimed_versions{0};
        uint64_t stale_index_deletes{0};
        uint64_t physical_deletes{0};
        uint64_t retained_txn_releases{0};
        uint64_t throttle_waits{0};
        uint64_t worker_exits{0};
        uint64_t pending_requests{0};
    };

    struct GarbageCollectionCounters {
        std::atomic<uint64_t> requests{0};
        std::atomic<uint64_t> attempts{0};
        std::atomic<uint64_t> worker_wakeups{0};
        std::atomic<uint64_t> reclaimed_versions{0};
        std::atomic<uint64_t> stale_index_deletes{0};
        std::atomic<uint64_t> physical_deletes{0};
        std::atomic<uint64_t> retained_txn_releases{0};
        std::atomic<uint64_t> throttle_waits{0};
        std::atomic<uint64_t> worker_exits{0};
        // pending_requests is protected by gc_worker_mutex_, not atomic.
        uint64_t pending_requests{0};
    };

    GarbageCollectionStats GetGarbageCollectionStats();

    void RequestGarbageCollection();

    void StopGarbageCollectionWorker();

    bool IsGarbageCollectionWorkerStopped();

    /** @brief Execute one garbage collection pass. */
    void GarbageCollection();

    class WriteTxnGuard {
       public:
        explicit WriteTxnGuard(TransactionManager *txn_mgr) {
            if (txn_mgr != nullptr) {
                checkpoint_guard_ = std::shared_lock<std::shared_mutex>(txn_mgr->checkpoint_mutex_);
            }
        }

       private:
        std::shared_lock<std::shared_mutex> checkpoint_guard_;
    };

    WriteTxnGuard write_txn_guard() {
        return WriteTxnGuard(this);
    }
    
    struct PageVersionInfo {
        std::shared_mutex mutex_;
        /** 存储所有槽的先前版本信息。注意：不要使用 `[x]` 来访问它，因为
         * 即使不存在也会创建新元素。请使用 `find` 来代替。
         */
        std::unordered_map<slot_offset_t, VersionUndoLink> prev_version_;
    };

    /** 保护版本信息 */
    std::shared_mutex version_info_mutex_;
    /** 存储表堆中每个元组的先前版本。 */
    struct VersionPageKey {
        int fd;
        page_id_t page_no;

        bool operator==(const VersionPageKey &other) const {
            return fd == other.fd && page_no == other.page_no;
        }
    };

    struct VersionPageKeyHash {
        size_t operator()(const VersionPageKey &key) const {
            auto lhs = static_cast<uint64_t>(static_cast<uint32_t>(key.fd));
            auto rhs = static_cast<uint64_t>(static_cast<uint32_t>(key.page_no));
            return std::hash<uint64_t>()((lhs << 32) | rhs);
        }
    };

    std::unordered_map<VersionPageKey, std::shared_ptr<PageVersionInfo>, VersionPageKeyHash> version_info_;

    bool UpdateVersionLink(int fd, Rid rid, std::optional<VersionUndoLink> prev_version,
                           std::function<bool(std::optional<VersionUndoLink>)> &&check = nullptr);

    std::optional<VersionUndoLink> GetVersionLink(int fd, Rid rid);

    void CleanupTransaction(Transaction *txn);


private:
    std::atomic<txn_id_t> next_txn_id_{0};  // 用于分发事务ID
    std::atomic<timestamp_t> next_timestamp_{0};    // 用于分发事务时间戳
    // Lock order: checkpoint_mutex_ -> watermark_mutex_ -> txn_map_mutex_ ->
    // version_info_mutex_ -> PageVersionInfo::mutex_ -> SmManager metadata lock.
    // A GC worker must release gc_worker_mutex_ before calling RunGarbageCollection.
    std::shared_mutex checkpoint_mutex_;
    std::shared_mutex txn_map_mutex_;  // 保护 txn_map 的并发访问
    std::mutex watermark_mutex_;       // 保护 running_txns_ 的并发访问
    SmManager *sm_manager_;
    LockManager *lock_manager_;

    std::atomic<timestamp_t> last_commit_ts_{0};    // 最后提交的时间戳,仅用于MVCC
    Watermark running_txns_{0};             // 存储所有正在运行事务的读取时间戳，以便于垃圾回收，仅用于MVCC

    // Protects worker state only. It is never held with GC data-structure locks.
    std::mutex gc_worker_mutex_;
    std::condition_variable gc_worker_cv_;
    std::thread gc_worker_;
    bool gc_worker_stop_{false};
    GarbageCollectionCounters gc_counters_;

    /**
     * 事务条目，存储事务的所有权和保留状态。
     */
    struct TxnEntry {
        std::unique_ptr<Transaction> txn;   // 事务所有权
        bool retained = false;              // 事务是否被保留，用于垃圾回收

        Transaction *Get() const { return txn.get(); }
    };
    // 存储所有事务的映射，键为事务ID，值为事务条目
    std::unordered_map<txn_id_t, TxnEntry> txn_map;
    
    void RemoveSsiEdgesForTxnLocked(txn_id_t txn_id);

    Transaction *GetTransactionLocked(txn_id_t txn_id);

    void RegisterTransactionLocked(std::unique_ptr<Transaction> txn);

    void SetTransactionRetainedLocked(txn_id_t txn_id, bool retained);

    void RemoveTransactionLocked(txn_id_t txn_id, std::vector<std::unique_ptr<Transaction>> *released_txns = nullptr);

    template <typename Callback>
    void ForEachTransactionLocked(Callback &&callback) {
        for (auto &[_, entry] : txn_map) {
            auto *other = entry.Get();
            if (other != nullptr) {
                callback(other);
            }
        }
    }

    // Builds the undo log for a write operation, inheriting the undo chain from the
    // current version link when the same transaction already owns the slot.
    UndoLog BuildUndoLogForWrite(int fd, const Rid &rid, const RmRecord &base_record, Transaction *txn);

    void StartGarbageCollectionWorker();

    void GarbageCollectionWorkerLoop();

    void RunGarbageCollection(bool apply_throttle);

    bool AddRwDependency(Transaction *from, Transaction *to, Transaction *current, const std::string &reason);

    bool HasDangerousStructure(Transaction *from, Transaction *to);
};
