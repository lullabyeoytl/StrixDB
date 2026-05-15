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

#include <iostream>

#include "index/ix.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

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

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    // 1. 判断传入事务参数是否为空指针
    // 2. 如果为空指针，创建新事务
    // 3. 把开始事务加入到全局事务表中
    // 4. 返回当前事务指针
    // 如果需要支持MVCC请在上述过程中添加代码
    if (txn == nullptr) {
        txn = new Transaction(next_txn_id_++);
    }
    txn->set_state(TransactionState::GROWING);
    txn->set_start_ts(next_timestamp_++);

    if (log_manager != nullptr) {
        BeginLogRecord log_record(txn->get_transaction_id());
        log_record.prev_lsn_ = txn->get_prev_lsn(); // record the previous lsn
        lsn_t lsn = log_manager->add_log_to_buffer(&log_record);
        // now update prev_lsn_ to this log's lsn
        txn->set_prev_lsn(lsn);
    }

    std::unique_lock<std::mutex> lock(latch_);
    txn_map[txn->get_transaction_id()] = txn;
    return txn;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction *txn, LogManager *log_manager) {
    // 1. 如果存在未提交的写操作，提交所有的写操作
    // 2. 释放所有锁
    // 3. 释放事务相关资源，eg.锁集
    // 4. 把事务日志刷入磁盘中
    // 5. 更新事务状态
    // 如果需要支持MVCC请在上述过程中添加代码
    if (txn == nullptr) {
        return;
    }
    if (log_manager != nullptr) {
        CommitLogRecord log_record(txn->get_transaction_id());
        log_record.prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = log_manager->add_log_to_buffer(&log_record);
        txn->set_prev_lsn(lsn);
        log_manager->flush_log_to_disk();
    }
    txn->set_state(TransactionState::SHRINKING);
    release_locks(txn, lock_manager_);
    delete_write_set(txn);
    txn->set_state(TransactionState::COMMITTED);

    std::unique_lock<std::mutex> lock(latch_);
    txn_map.erase(txn->get_transaction_id());
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction *txn, LogManager *log_manager) {
    // 1. 回滚所有写操作
    // 2. 释放所有锁
    // 3. 清空事务相关资源，eg.锁集
    // 4. 把事务日志刷入磁盘中
    // 5. 更新事务状态
    // 如果需要支持MVCC请在上述过程中添加代码
    if (txn == nullptr) {
        return;
    }
    txn->set_state(TransactionState::ABORTED);
    auto &write_set = *txn->get_write_set();
    for (auto it = write_set.rbegin(); it != write_set.rend(); ++it) {
        WriteRecord *write_record = *it;
        try {
            const std::string &tab_name = write_record->GetTableName();
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
                fh->insert_record(write_record->GetRid(), write_record->GetRecord().data, txn->get_prev_lsn());
                apply_index_op(sm_manager_, tab_name, tab, write_record->GetRecord(), write_record->GetRid(), txn,
                               IndexEntryOp::INSERT);
            } else if (write_record->GetWriteType() == WType::UPDATE_TUPLE) {
                auto current_rec = fh->get_record(write_record->GetRid(), nullptr);
                UpdateLogRecord clr(txn->get_transaction_id(), *current_rec, write_record->GetRecord(),
                                    write_record->GetRid(), tab_name);
                append_txn_clr(txn, log_manager, clr, LogType::CLR_UPDATE, write_record->GetOpPrevLsn());
                apply_index_op(sm_manager_, tab_name, tab, *current_rec, write_record->GetRid(), txn,
                               IndexEntryOp::DELETE);
                apply_index_op(sm_manager_, tab_name, tab, write_record->GetRecord(), write_record->GetRid(), txn,
                               IndexEntryOp::INSERT);
                fh->update_record(write_record->GetRid(), write_record->GetRecord().data, nullptr,
                                  txn->get_prev_lsn());
            }
        } catch (const std::exception &error) {
            UndoError undo_error(txn->get_transaction_id(), write_record->GetTableName(), write_record->GetRid(),
                                 error.what());
            std::cerr << undo_error.what() << std::endl;
        } catch (...) {
            UndoError undo_error(txn->get_transaction_id(), write_record->GetTableName(), write_record->GetRid(),
                                 "unknown exception");
            std::cerr << undo_error.what() << std::endl;
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
    release_locks(txn, lock_manager_);
    delete_write_set(txn);
    txn->set_state(TransactionState::ABORTED);

    std::unique_lock<std::mutex> lock(latch_);
    txn_map.erase(txn->get_transaction_id());
}
