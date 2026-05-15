/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

namespace {

constexpr bool COMPAT[5][5] = {
    {true, false, true, false, false},
    {false, false, false, false, false},
    {true, false, true, true, true},
    {false, false, true, true, false},
    {false, false, true, false, false},
};

}  // namespace

inline int LockManager::lock_mode_index(LockMode mode) const {
    switch (mode) {
        case LockMode::SHARED:
            return 0;
        case LockMode::EXLUCSIVE:
            return 1;
        case LockMode::INTENTION_SHARED:
            return 2;
        case LockMode::INTENTION_EXCLUSIVE:
            return 3;
        case LockMode::S_IX:
            return 4;
    }
    return 0;
}

bool LockManager::is_compatible(const LockRequestQueue &queue, txn_id_t requester_id, LockMode mode) const {
    int requested = lock_mode_index(mode);
    for (const auto &request : queue.request_queue_) {
        if (!request.granted_ || request.txn_id_ == requester_id) {
            continue;
        }
        if (!COMPAT[lock_mode_index(request.lock_mode_)][requested]) {
            return false;
        }
    }
    return true;
}

bool LockManager::has_prior_upgrade_request(const LockRequestQueue &queue, txn_id_t requester_id) const {
    for (const auto &request : queue.request_queue_) {
        if (request.txn_id_ == requester_id) {
            return false;
        }
        // 自身想获取锁但存在未完成升级请求
        if (request.is_upgrade_ && !request.granted_) {
            return true;
        }
    }
    return false;
}

bool LockManager::can_wait(const LockRequestQueue &queue, txn_id_t requester_id) const {
    for (const auto &request : queue.request_queue_) {
        if (!request.granted_ || request.txn_id_ == requester_id) {
            continue;
        }
        // 存在比自己更早的未完成升级请求，无法等待
        if (requester_id > request.txn_id_) {
            return false;
        }
    }
    return true;
}

// 已经持有的锁是否能覆盖本次请求
bool LockManager::is_at_least_as_strong(LockMode held, LockMode requested) const {
    if (held == requested || held == LockMode::EXLUCSIVE) {
        return true;
    }
    if (held == LockMode::S_IX) {
        return requested == LockMode::SHARED || requested == LockMode::INTENTION_SHARED ||
               requested == LockMode::INTENTION_EXCLUSIVE;
    }
    if (held == LockMode::SHARED) {
        return requested == LockMode::INTENTION_SHARED;
    }
    if (held == LockMode::INTENTION_EXCLUSIVE) {
        return requested == LockMode::INTENTION_SHARED;
    }
    return false;
}

// void LockManager::recompute_group_mode(LockRequestQueue &queue) {
//     bool has_s = false;
//     bool has_x = false;
//     bool has_is = false;
//     bool has_ix = false;
//     bool has_six = false;
//     for (const auto &request : queue.request_queue_) {
//         if (!request.granted_) {
//             continue;
//         }
//         switch (request.lock_mode_) {
//             case LockMode::SHARED:
//                 has_s = true;
//                 break;
//             case LockMode::EXLUCSIVE:
//                 has_x = true;
//                 break;
//             case LockMode::INTENTION_SHARED:
//                 has_is = true;
//                 break;
//             case LockMode::INTENTION_EXCLUSIVE:
//                 has_ix = true;
//                 break;
//             case LockMode::S_IX:
//                 has_six = true;
//                 break;
//         }
//     }

//     if (has_x) {
//         queue.group_lock_mode_ = GroupLockMode::X;
//     } else if (has_six || (has_s && has_ix)) {
//         queue.group_lock_mode_ = GroupLockMode::SIX;
//     } else if (has_s) {
//         queue.group_lock_mode_ = GroupLockMode::S;
//     } else if (has_ix) {
//         queue.group_lock_mode_ = GroupLockMode::IX;
//     } else if (has_is) {
//         queue.group_lock_mode_ = GroupLockMode::IS;
//     } else {
//         queue.group_lock_mode_ = GroupLockMode::NON_LOCK;
//     }
// }

/**
 * @description: 表/行锁统一入口, 使用wait-die策略
 * @return: 是否成功获取锁
 * @param lock: 锁的锁对象
 * @param queue: 锁请求队列
 * @param txn: 事务对象
 * @param mode: 锁模式
 * @param lock_data_id: 锁数据ID
 */
bool LockManager::acquire_lock(std::unique_lock<std::mutex> &lock, LockRequestQueue &queue, Transaction *txn,
                               LockMode mode, const LockDataId &lock_data_id) {
    if (txn == nullptr) {
        return true;
    }
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    txn_id_t tid = txn->get_transaction_id();
    for (auto request_it = queue.request_queue_.begin(); request_it != queue.request_queue_.end(); ++request_it) {
        if (request_it->txn_id_ != tid || !request_it->granted_) {
            continue;
        }
        if (is_at_least_as_strong(request_it->lock_mode_, mode)) {
            txn->get_lock_set()->emplace(lock_data_id);
            return true;
        }

        return perform_upgrade(lock, queue, txn, request_it, mode, lock_data_id);
    }

    return acquire_new_lock(lock, queue, txn, mode, lock_data_id);
}

bool LockManager::perform_upgrade(std::unique_lock<std::mutex> &lock, LockRequestQueue &queue, Transaction *txn,
                                  std::list<LockRequest>::iterator request_it, LockMode mode,
                                  const LockDataId &lock_data_id) {
    txn_id_t tid = txn->get_transaction_id();
    if (queue.upgrading_) {
        throw TransactionAbortException(tid, AbortReason::UPGRADE_CONFLICT);
    }

    queue.upgrading_ = true;
    auto upgrade_it = queue.request_queue_.emplace(std::next(request_it), tid, mode, true);

    while (!is_compatible(queue, tid, mode)) {
        if (!can_wait(queue, tid)) {
            queue.request_queue_.erase(upgrade_it);
            queue.upgrading_ = false;
            queue.cv_.notify_all();
            throw TransactionAbortException(tid, AbortReason::DEADLOCK_PREVENTION);
        }
        queue.cv_.wait(lock);
    }

    request_it->lock_mode_ = mode;
    queue.request_queue_.erase(upgrade_it);
    queue.upgrading_ = false;
    txn->get_lock_set()->emplace(lock_data_id);
    txn->set_state(TransactionState::GROWING);
    queue.cv_.notify_all();
    return true;
}

bool LockManager::acquire_new_lock(std::unique_lock<std::mutex> &lock, LockRequestQueue &queue, Transaction *txn,
                                   LockMode mode, const LockDataId &lock_data_id) {
    txn_id_t tid = txn->get_transaction_id();
    queue.request_queue_.emplace_back(tid, mode);
    auto request_it = std::prev(queue.request_queue_.end());
    while (has_prior_upgrade_request(queue, tid) || !is_compatible(queue, tid, mode)) {
        if (!can_wait(queue, tid)) {
            queue.request_queue_.erase(request_it);
            queue.cv_.notify_all();
            throw TransactionAbortException(tid, AbortReason::DEADLOCK_PREVENTION);
        }
        queue.cv_.wait(lock);
    }

    request_it->granted_ = true;
    txn->get_lock_set()->emplace(lock_data_id);
    txn->set_state(TransactionState::GROWING);
    queue.cv_.notify_all();
    return true;
}

/**
 * @description: 获取记录共享锁
 * @return: 是否成功获取锁
 * @param txn: 事务对象
 * @param rid: 记录ID
 * @param tab_fd: 表文件描述符
 */
bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    if (!lock_IS_on_table(txn, tab_fd)) {
        return false;
    }
    std::unique_lock<std::mutex> lock(latch_);
    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    return acquire_lock(lock, lock_table_[lock_data_id], txn, LockMode::SHARED, lock_data_id);
}

/**
 * @description: 获取记录排他锁
 * @return: 是否成功获取锁
 * @param txn: 事务对象
 * @param rid: 记录ID
 * @param tab_fd: 表文件描述符
 */
bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    if (!lock_IX_on_table(txn, tab_fd)) {
        return false;
    }
    std::unique_lock<std::mutex> lock(latch_);
    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    return acquire_lock(lock, lock_table_[lock_data_id], txn, LockMode::EXLUCSIVE, lock_data_id);
}

/**
 * @description: 获取表共享锁
 * @return: 是否成功获取锁
 * @param txn: 事务对象
 * @param tab_fd: 表文件描述符
 */
bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    std::unique_lock<std::mutex> lock(latch_);
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    return acquire_lock(lock, lock_table_[lock_data_id], txn, LockMode::SHARED, lock_data_id);
}

/**
 * @description: 获取表排他锁
 * @return: 是否成功获取锁
 * @param txn: 事务对象
 * @param tab_fd: 表文件描述符
 */
bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    std::unique_lock<std::mutex> lock(latch_);
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    return acquire_lock(lock, lock_table_[lock_data_id], txn, LockMode::EXLUCSIVE, lock_data_id);
}

/**
 * @description: 获取表意向共享锁
 * @return: 是否成功获取锁
 * @param txn: 事务对象
 * @param tab_fd: 表文件描述符
 */
bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    std::unique_lock<std::mutex> lock(latch_);
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    return acquire_lock(lock, lock_table_[lock_data_id], txn, LockMode::INTENTION_SHARED, lock_data_id);
}

/**
 * @description: 获取表意向排他锁
 * @return: 是否成功获取锁
 * @param txn: 事务对象
 * @param tab_fd: 表文件描述符
 */
bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    std::unique_lock<std::mutex> lock(latch_);
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    return acquire_lock(lock, lock_table_[lock_data_id], txn, LockMode::INTENTION_EXCLUSIVE, lock_data_id);
}

/**
 * @description: 解锁
 * @return: 是否成功解锁
 * @param txn: 事务对象
 * @param lock_data_id: 锁数据ID
 */
bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    std::unique_lock<std::mutex> lock(latch_);
    auto table_it = lock_table_.find(lock_data_id);
    if (table_it == lock_table_.end()) {
        if (txn != nullptr) {
            txn->get_lock_set()->erase(lock_data_id);
        }
        return true;
    }

    txn_id_t tid = txn == nullptr ? INVALID_TXN_ID : txn->get_transaction_id();
    auto &queue = table_it->second;
    for (auto it = queue.request_queue_.begin(); it != queue.request_queue_.end();) {
        if (it->txn_id_ == tid) {
            it = queue.request_queue_.erase(it);
        } else {
            ++it;
        }
    }
    if (txn != nullptr) {
        txn->get_lock_set()->erase(lock_data_id);
    }
    // recompute_group_mode(queue);
    queue.cv_.notify_all();
    if (queue.request_queue_.empty()) {
        lock_table_.erase(table_it);
    }
    return true;
}
