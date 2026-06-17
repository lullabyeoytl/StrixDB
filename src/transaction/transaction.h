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

#include <deque>
#include <memory>
#include <unordered_set>

#include "common/common.h"
#include "transaction/txn_defs.h"

class Transaction : public NonCopyable {
public:
    explicit Transaction(txn_id_t txn_id) : txn_id_(txn_id) {
        write_set_ = std::make_shared<std::deque<WriteRecord *>>();
        lock_set_ = std::make_shared<std::unordered_set<LockDataId>>();
    }

    ~Transaction() = default;

    inline txn_id_t get_transaction_id() const { return txn_id_; }

    inline void set_txn_mode(bool txn_mode) { txn_mode_ = txn_mode; }
    inline bool get_txn_mode() const { return txn_mode_; }

    inline TransactionState get_state() const { return state_; }
    inline void set_state(TransactionState state) { state_ = state; }

    inline lsn_t get_prev_lsn() const { return prev_lsn_; }
    inline void set_prev_lsn(lsn_t prev_lsn) { prev_lsn_ = prev_lsn; }

    inline std::shared_ptr<std::deque<WriteRecord *>> get_write_set() { return write_set_; }
    inline void append_write_record(WriteRecord *write_record) { write_set_->push_back(write_record); }

    inline std::shared_ptr<std::unordered_set<LockDataId>> get_lock_set() { return lock_set_; }

private:
    bool txn_mode_{false};
    TransactionState state_{TransactionState::DEFAULT};
    lsn_t prev_lsn_{INVALID_LSN};
    txn_id_t txn_id_{INVALID_TXN_ID};

    std::shared_ptr<std::deque<WriteRecord *>> write_set_;
    std::shared_ptr<std::unordered_set<LockDataId>> lock_set_;
};
