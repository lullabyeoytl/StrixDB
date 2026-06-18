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
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "common/exception.h"
#include "concurrency/lock_manager.h"
#include "recovery/log_manager.h"
#include "system/sm_manager.h"
#include "transaction.h"

class TransactionManager : public NonCopyable {
public:
    explicit TransactionManager(LockManager *lock_manager, SmManager *sm_manager)
        : sm_manager_(sm_manager), lock_manager_(lock_manager) {}

    ~TransactionManager();

    Transaction *begin(Transaction *txn, LogManager *log_manager,
                       IsolationLevel isolation_level = IsolationLevel::SERIALIZABLE);

    void commit(Transaction *txn, LogManager *log_manager);

    void abort(Transaction *txn, LogManager *log_manager);

    lsn_t create_static_checkpoint(LogManager *log_manager);

    LockManager *get_lock_manager() { return lock_manager_; }

    Transaction *get_transaction(txn_id_t txn_id) {
        if (txn_id == INVALID_TXN_ID) {
            return nullptr;
        }

        std::shared_lock<std::shared_mutex> lock(txn_map_mutex_);
        return GetTransactionLocked(txn_id);
    }

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

    WriteTxnGuard write_txn_guard() { return WriteTxnGuard(this); }

private:
    struct TxnEntry {
        std::unique_ptr<Transaction> txn;

        Transaction *Get() const { return txn.get(); }
    };

    std::atomic<txn_id_t> next_txn_id_{0};
    std::shared_mutex checkpoint_mutex_;
    std::shared_mutex txn_map_mutex_;
    SmManager *sm_manager_;
    LockManager *lock_manager_;
    std::unordered_map<txn_id_t, TxnEntry> txn_map;

    Transaction *GetTransactionLocked(txn_id_t txn_id);

    void RegisterTransactionLocked(std::unique_ptr<Transaction> txn);

    void RemoveTransactionLocked(txn_id_t txn_id, std::vector<std::unique_ptr<Transaction>> *released_txns = nullptr);
};
