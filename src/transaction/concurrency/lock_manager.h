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

#include <condition_variable>
#include <list>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "transaction/transaction.h"

static const std::string GroupLockModeStr[6] = {"NON_LOCK", "IS", "IX", "S", "X", "SIX"};

class LockManager: public NonCopyable {
    /* 加锁类型，包括共享锁、排他锁、意向共享锁、意向排他锁、SIX（意向排他锁+共享锁） */
    enum class LockMode { SHARED, EXLUCSIVE, INTENTION_SHARED, INTENTION_EXCLUSIVE, S_IX };

    /* 用于标识加锁队列中排他性最强的锁类型，例如加锁队列中有SHARED和EXLUSIVE两个加锁操作，则该队列的锁模式为X */
    enum class GroupLockMode { NON_LOCK, IS, IX, S, X, SIX};

    /* 事务的加锁申请 */
    class LockRequest {
    public:
        LockRequest(txn_id_t txn_id, LockMode lock_mode)
            : txn_id_(txn_id), lock_mode_(lock_mode), granted_(false) {}
        LockRequest(txn_id_t txn_id, LockMode lock_mode, bool is_upgrade)
            : txn_id_(txn_id), lock_mode_(lock_mode), granted_(false), is_upgrade_(is_upgrade) {}

        txn_id_t txn_id_;   // 申请加锁的事务ID
        LockMode lock_mode_;    // 事务申请加锁的类型
        bool granted_;          // 该事务是否已经被赋予锁
        bool is_upgrade_ = false;   // 是否是升级锁
    };

    /* 数据项上的加锁队列 */
    class LockRequestQueue {
    public:
        std::list<LockRequest> request_queue_;  // 加锁队列
        std::condition_variable cv_;            // 条件变量，用于唤醒正在等待加锁的申请，在no-wait策略下无需使用
        // GroupLockMode group_lock_mode_ = GroupLockMode::NON_LOCK;   // 加锁队列的锁模式
        bool upgrading_ = false;    // 是否有进行中的升级操作
    };

public:
    LockManager() {}

    ~LockManager() {}

    bool lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd);

    bool lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd);

    bool lock_shared_on_table(Transaction* txn, int tab_fd);

    bool lock_exclusive_on_table(Transaction* txn, int tab_fd);

    bool lock_IS_on_table(Transaction* txn, int tab_fd);

    bool lock_IX_on_table(Transaction* txn, int tab_fd);

    bool unlock(Transaction* txn, LockDataId lock_data_id);

private:
    struct WaitEdge {
        txn_id_t blocker_;
    };

    int lock_mode_index(LockMode mode) const;
    bool is_compatible(const LockRequestQueue &queue, txn_id_t requester_id, LockMode mode) const;
    bool should_wait(const LockRequestQueue &queue, txn_id_t requester_id, LockMode mode) const;
    bool should_wait_for_conflict(Transaction *txn, const LockDataId &lock_data_id) const;
    bool is_at_least_as_strong(LockMode held, LockMode requested) const;
    std::vector<txn_id_t> collect_wait_blockers(const LockRequestQueue &queue, txn_id_t requester_id,
                                                LockMode mode) const;
    bool has_wait_path(txn_id_t current, txn_id_t target, std::unordered_set<txn_id_t> &visited) const;
    bool has_wait_cycle(txn_id_t txn_id) const;
    void set_wait_edges(txn_id_t waiter, const std::vector<txn_id_t> &blockers);
    void clear_waiter_edges(txn_id_t waiter);
    void clear_blocker_edges(txn_id_t blocker);
    // void recompute_group_mode(LockRequestQueue &queue);
    bool acquire_lock(std::unique_lock<std::mutex> &lock, LockRequestQueue &queue, Transaction *txn, LockMode mode,
                      const LockDataId &lock_data_id);
    bool acquire_new_lock(std::unique_lock<std::mutex> &lock, LockRequestQueue &queue, Transaction *txn, LockMode mode,
                          const LockDataId &lock_data_id);
    bool perform_upgrade(std::unique_lock<std::mutex> &lock, LockRequestQueue &queue, Transaction *txn,
                         std::list<LockRequest>::iterator request_it, LockMode mode,
                         const LockDataId &lock_data_id);

    std::mutex latch_;      // 用于锁表的并发
    std::unordered_map<LockDataId, LockRequestQueue> lock_table_;   // 全局锁表
    std::unordered_map<txn_id_t, std::vector<WaitEdge>> wait_edges_;
};
