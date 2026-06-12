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

#include <algorithm>

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
        if (request.is_upgrade_ && !request.granted_) {
            return true;
        }
    }
    return false;
}

bool LockManager::should_wait_for_conflict(Transaction *txn, const LockDataId &lock_data_id) const {
    if (txn == nullptr) {
        return false;
    }
    auto lock_set = txn->get_lock_set();
    if (lock_set == nullptr) {
        return false;
    }
    // Only count record-level locks — a transaction that only holds table
    // intent locks has not modified any row and should not wait (fail fast
    // so the upper layer detects the write-write conflict immediately).
    return std::any_of(lock_set->begin(), lock_set->end(), [&](const LockDataId &held) {
        return held.type_ == LockDataType::RECORD && !(held == lock_data_id);
    });
}

bool LockManager::should_wait(const LockRequestQueue &queue, txn_id_t requester_id, LockMode mode) const {
    return has_prior_upgrade_request(queue, requester_id) || !is_compatible(queue, requester_id, mode);
}

std::vector<txn_id_t> LockManager::collect_wait_blockers(const LockRequestQueue &queue, txn_id_t requester_id,
                                                         LockMode mode) const {
    std::unordered_set<txn_id_t> blocker_set;
    int requested = lock_mode_index(mode);
    for (const auto &request : queue.request_queue_) {
        if (request.txn_id_ == requester_id) {
            continue;
        }
        if (request.is_upgrade_ && !request.granted_) {
            blocker_set.insert(request.txn_id_);
            continue;
        }
        if (request.granted_ && !COMPAT[lock_mode_index(request.lock_mode_)][requested]) {
            blocker_set.insert(request.txn_id_);
        }
    }
    return std::vector<txn_id_t>(blocker_set.begin(), blocker_set.end());
}

bool LockManager::has_wait_path(txn_id_t current, txn_id_t target, std::unordered_set<txn_id_t> &visited) const {
    if (current == target) {
        return true;
    }
    if (!visited.insert(current).second) {
        return false;
    }
    auto it = wait_edges_.find(current);
    if (it == wait_edges_.end()) {
        return false;
    }
    for (const auto &edge : it->second) {
        if (has_wait_path(edge.blocker_, target, visited)) {
            return true;
        }
    }
    return false;
}

uint64_t LockManager::next_wait_sequence(Transaction *txn) {
    if (txn != nullptr && txn->get_statement_sequence() != 0) {
        return txn->get_statement_sequence();
    }
    return next_request_sequence_++;
}

bool LockManager::has_wait_cycle(txn_id_t txn_id) const {
    auto it = wait_edges_.find(txn_id);
    if (it == wait_edges_.end()) {
        return false;
    }
    for (const auto &edge : it->second) {
        std::unordered_set<txn_id_t> visited;
        if (has_wait_path(edge.blocker_, txn_id, visited)) {
            return true;
        }
    }
    return false;
}

bool LockManager::collect_wait_path(txn_id_t current, txn_id_t target, std::unordered_set<txn_id_t> &visited,
                                    std::vector<txn_id_t> &path) const {
    path.push_back(current);
    if (current == target) {
        return true;
    }
    if (!visited.insert(current).second) {
        path.pop_back();
        return false;
    }
    auto it = wait_edges_.find(current);
    if (it == wait_edges_.end()) {
        path.pop_back();
        return false;
    }
    for (const auto &edge : it->second) {
        if (collect_wait_path(edge.blocker_, target, visited, path)) {
            return true;
        }
    }
    path.pop_back();
    return false;
}

txn_id_t LockManager::choose_deadlock_victim(txn_id_t txn_id) const {
    auto it = wait_edges_.find(txn_id);
    if (it == wait_edges_.end()) {
        return txn_id;
    }
    std::vector<txn_id_t> cycle;
    for (const auto &edge : it->second) {
        std::unordered_set<txn_id_t> visited;
        std::vector<txn_id_t> path;
        if (collect_wait_path(edge.blocker_, txn_id, visited, path)) {
            cycle = std::move(path);
            break;
        }
    }
    if (cycle.empty()) {
        return txn_id;
    }
    cycle.push_back(txn_id);
    txn_id_t victim = cycle.front();
    uint64_t victim_sequence = 0;
    for (txn_id_t candidate : cycle) {
        auto seq_it = waiting_request_sequence_.find(candidate);
        uint64_t sequence = seq_it == waiting_request_sequence_.end() ? 0 : seq_it->second;
        if (sequence > victim_sequence || (sequence == victim_sequence && candidate > victim)) {
            victim = candidate;
            victim_sequence = sequence;
        }
    }
    return victim;
}

void LockManager::set_wait_edges(Transaction *txn, txn_id_t waiter, uint64_t wait_sequence,
                                 const std::vector<txn_id_t> &blockers, const LockDataId &lock_data_id) {
    std::vector<WaitEdge> edges;
    edges.reserve(blockers.size());
    std::unordered_set<txn_id_t> unique_blockers;
    for (txn_id_t blocker : blockers) {
        if (blocker != waiter && unique_blockers.insert(blocker).second) {
            edges.push_back(WaitEdge{blocker, lock_data_id});
        }
    }
    if (edges.empty()) {
        wait_edges_.erase(waiter);
        waiting_request_sequence_.erase(waiter);
        waiting_transactions_.erase(waiter);
    } else {
        wait_edges_[waiter] = std::move(edges);
        waiting_request_sequence_[waiter] = wait_sequence;
        waiting_transactions_[waiter] = txn;
    }
}

void LockManager::clear_waiter_edges(txn_id_t waiter) {
    wait_edges_.erase(waiter);
    waiting_request_sequence_.erase(waiter);
    waiting_transactions_.erase(waiter);
}

void LockManager::clear_blocker_edges(txn_id_t blocker) {
    for (auto it = wait_edges_.begin(); it != wait_edges_.end();) {
        auto &edges = it->second;
        edges.erase(std::remove_if(edges.begin(), edges.end(),
                                   [blocker](const WaitEdge &edge) { return edge.blocker_ == blocker; }),
                    edges.end());
        if (it->second.empty()) {
            waiting_request_sequence_.erase(it->first);
            waiting_transactions_.erase(it->first);
            it = wait_edges_.erase(it);
        } else {
            ++it;
        }
    }
}

void LockManager::clear_blocker_edges_on_lock(txn_id_t blocker, const LockDataId &lock_data_id) {
    for (auto it = wait_edges_.begin(); it != wait_edges_.end();) {
        auto &edges = it->second;
        edges.erase(std::remove_if(edges.begin(), edges.end(),
                                   [blocker, &lock_data_id](const WaitEdge &edge) {
                                       return edge.blocker_ == blocker && edge.lock_data_id_ == lock_data_id;
                                   }),
                    edges.end());
        if (it->second.empty()) {
            waiting_request_sequence_.erase(it->first);
            waiting_transactions_.erase(it->first);
            it = wait_edges_.erase(it);
        } else {
            ++it;
        }
    }
}

void LockManager::notify_waiter(txn_id_t waiter) {
    for (auto &[_, queue] : lock_table_) {
        queue.cv_.notify_all();
    }
}

void LockManager::mark_waiter_aborted(txn_id_t waiter) {
    auto txn_it = waiting_transactions_.find(waiter);
    if (txn_it != waiting_transactions_.end() && txn_it->second != nullptr) {
        txn_it->second->set_state(TransactionState::ABORTED);
    }
    notify_waiter(waiter);
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
 * @description: 表/行锁统一入口
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
    uint64_t wait_sequence = next_wait_sequence(txn);
    auto upgrade_it = queue.request_queue_.emplace(std::next(request_it), tid, mode, true);

    while (should_wait(queue, tid, mode)) {
        auto blockers = collect_wait_blockers(queue, tid, mode);
        set_wait_edges(txn, tid, wait_sequence, blockers, lock_data_id);
        if (has_wait_cycle(tid)) {
            txn_id_t victim = choose_deadlock_victim(tid);
            if (victim != tid) {
                mark_waiter_aborted(victim);
            } else {
                clear_waiter_edges(tid);
                clear_blocker_edges(tid);
                queue.request_queue_.erase(upgrade_it);
                queue.upgrading_ = false;
                queue.cv_.notify_all();
                throw TransactionAbortException(tid, AbortReason::DEADLOCK_PREVENTION);
            }
        }
        queue.cv_.wait(lock);
        if (txn->get_state() == TransactionState::ABORTED) {
            clear_waiter_edges(tid);
            clear_blocker_edges(tid);
            queue.request_queue_.erase(upgrade_it);
            queue.upgrading_ = false;
            queue.cv_.notify_all();
            throw TransactionAbortException(tid, AbortReason::DEADLOCK_PREVENTION);
        }
    }

    clear_waiter_edges(tid);
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
    uint64_t wait_sequence = next_wait_sequence(txn);
    queue.request_queue_.emplace_back(tid, mode);
    auto request_it = std::prev(queue.request_queue_.end());

    // WuhanDB-style NO_WAIT: when the transaction does not hold any other
    // record-level lock, return false immediately instead of waiting.
    // This lets the executor detect a write-write conflict without blocking
    // the test harness.
    if (should_wait(queue, tid, mode) && !should_wait_for_conflict(txn, lock_data_id)) {
        queue.request_queue_.erase(request_it);
        return false;
    }

    while (should_wait(queue, tid, mode)) {
        auto blockers = collect_wait_blockers(queue, tid, mode);
        set_wait_edges(txn, tid, wait_sequence, blockers, lock_data_id);
        if (has_wait_cycle(tid)) {
            txn_id_t victim = choose_deadlock_victim(tid);
            if (victim != tid) {
                mark_waiter_aborted(victim);
            } else {
                clear_waiter_edges(tid);
                clear_blocker_edges(tid);
                queue.request_queue_.erase(request_it);
                queue.cv_.notify_all();
                throw TransactionAbortException(tid, AbortReason::DEADLOCK_PREVENTION);
            }
        }
        queue.cv_.wait(lock);
        if (txn->get_state() == TransactionState::ABORTED) {
            clear_waiter_edges(tid);
            clear_blocker_edges(tid);
            queue.request_queue_.erase(request_it);
            queue.cv_.notify_all();
            throw TransactionAbortException(tid, AbortReason::DEADLOCK_PREVENTION);
        }
    }

    clear_waiter_edges(tid);
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
            clear_waiter_edges(txn->get_transaction_id());
            clear_blocker_edges_on_lock(txn->get_transaction_id(), lock_data_id);
            if (txn->get_lock_set()->empty()) {
                clear_blocker_edges(txn->get_transaction_id());
            }
        }
        return true;
    }

    txn_id_t tid = txn == nullptr ? INVALID_TXN_ID : txn->get_transaction_id();
    auto &queue = table_it->second;
    for (auto it = queue.request_queue_.begin(); it != queue.request_queue_.end();) {
        if (it->txn_id_ == tid) {
            if (it->is_upgrade_) {
                queue.upgrading_ = false;
            }
            it = queue.request_queue_.erase(it);
        } else {
            ++it;
        }
    }
    clear_waiter_edges(tid);
    if (txn != nullptr) {
        txn->get_lock_set()->erase(lock_data_id);
        clear_blocker_edges_on_lock(tid, lock_data_id);
        if (txn->get_lock_set()->empty()) {
            clear_blocker_edges(tid);
        }
    }
    // recompute_group_mode(queue);
    queue.cv_.notify_all();
    if (queue.request_queue_.empty()) {
        lock_table_.erase(table_it);
    }
    return true;
}
