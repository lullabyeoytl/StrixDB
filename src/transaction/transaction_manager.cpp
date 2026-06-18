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

#include <cerrno>
#include <cstring>
#include <iostream>
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

void delete_write_set(Transaction *txn) {
    if (txn == nullptr) {
        return;
    }
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
    if (txn == nullptr || log_manager == nullptr) {
        return;
    }
    clr.log_type_ = clr_type;
    clr.prev_lsn_ = txn->get_prev_lsn();
    clr.undo_next_lsn_ = undo_next_lsn;
    lsn_t lsn = log_manager->add_log_to_buffer(&clr);
    txn->set_prev_lsn(lsn);
}
}  // namespace

TransactionManager::~TransactionManager() = default;

Transaction *TransactionManager::GetTransactionLocked(txn_id_t txn_id) {
    auto it = txn_map.find(txn_id);
    if (it == txn_map.end()) {
        return nullptr;
    }
    return it->second.Get();
}

void TransactionManager::RegisterTransactionLocked(std::unique_ptr<Transaction> txn) {
    txn_id_t txn_id = txn->get_transaction_id();
    txn_map[txn_id] = TxnEntry{std::move(txn)};
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

Transaction *TransactionManager::begin(Transaction *txn, LogManager *log_manager, IsolationLevel) {
    std::unique_lock<std::shared_mutex> checkpoint_lock(checkpoint_mutex_);
    auto owned = std::make_unique<Transaction>(next_txn_id_++);
    txn = owned.get();
    txn->set_state(TransactionState::GROWING);
    {
        std::unique_lock<std::shared_mutex> txn_lock(txn_map_mutex_);
        RegisterTransactionLocked(std::move(owned));
    }

    if (log_manager != nullptr) {
        BeginLogRecord log_record(txn->get_transaction_id());
        log_record.prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = log_manager->add_log_to_buffer(&log_record);
        txn->set_prev_lsn(lsn);
    }

    return txn;
}

void TransactionManager::commit(Transaction *txn, LogManager *log_manager) {
    std::unique_lock<std::shared_mutex> checkpoint_lock(checkpoint_mutex_);
    if (txn == nullptr) {
        return;
    }

    txn_id_t txn_id = txn->get_transaction_id();
    if (log_manager != nullptr) {
        CommitLogRecord log_record(txn_id);
        log_record.prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = log_manager->add_log_to_buffer(&log_record);
        log_manager->flush_buffer_to_disk_safe();
        txn->set_prev_lsn(lsn);
    }

    txn->set_state(TransactionState::SHRINKING);
    release_locks(txn, lock_manager_);
    txn->set_state(TransactionState::COMMITTED);
    delete_write_set(txn);

    std::vector<std::unique_ptr<Transaction>> released_txns;
    {
        std::unique_lock<std::shared_mutex> txn_lock(txn_map_mutex_);
        RemoveTransactionLocked(txn_id, &released_txns);
    }
}

void TransactionManager::abort(Transaction *txn, LogManager *log_manager) {
    std::unique_lock<std::shared_mutex> checkpoint_lock(checkpoint_mutex_);
    if (txn == nullptr) {
        return;
    }

    txn_id_t txn_id = txn->get_transaction_id();
    txn->set_state(TransactionState::ABORTED);

    bool undo_failed = false;
    auto &write_set = *txn->get_write_set();
    for (auto it = write_set.rbegin(); it != write_set.rend(); ++it) {
        if (undo_failed) {
            break;
        }

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
                DeleteLogRecord clr(txn_id, *rec, write_record->GetRid(), tab_name);
                append_txn_clr(txn, log_manager, clr, LogType::CLR_DELETE, write_record->GetOpPrevLsn());
                apply_index_op(sm_manager_, tab_name, tab, *rec, write_record->GetRid(), txn, IndexEntryOp::DELETE);
                fh->delete_record(write_record->GetRid(), nullptr, txn->get_prev_lsn());
            } else if (write_record->GetWriteType() == WType::DELETE_TUPLE) {
                InsertLogRecord clr(txn_id, write_record->GetRecord(), write_record->GetRid(), tab_name);
                append_txn_clr(txn, log_manager, clr, LogType::CLR_INSERT, write_record->GetOpPrevLsn());
                if (fh->is_record(write_record->GetRid())) {
                    fh->update_record(write_record->GetRid(), write_record->GetRecord().data, nullptr,
                                      txn->get_prev_lsn());
                } else {
                    fh->insert_record(write_record->GetRid(), write_record->GetRecord().data, txn->get_prev_lsn());
                }
                apply_index_op(sm_manager_, tab_name, tab, write_record->GetRecord(), write_record->GetRid(), txn,
                               IndexEntryOp::INSERT);
            } else if (write_record->GetWriteType() == WType::UPDATE_TUPLE) {
                auto current_rec = fh->get_record(write_record->GetRid(), nullptr);
                UpdateLogRecord clr(txn_id, *current_rec, write_record->GetRecord(), write_record->GetRid(),
                                    tab_name);
                append_txn_clr(txn, log_manager, clr, LogType::CLR_UPDATE, write_record->GetOpPrevLsn());
                apply_index_op(sm_manager_, tab_name, tab, *current_rec, write_record->GetRid(), txn,
                               IndexEntryOp::DELETE);
                apply_index_op(sm_manager_, tab_name, tab, write_record->GetRecord(), write_record->GetRid(), txn,
                               IndexEntryOp::INSERT);
                fh->update_record(write_record->GetRid(), write_record->GetRecord().data, nullptr,
                                  txn->get_prev_lsn());
            }
        } catch (const std::exception &error) {
            UndoError undo_error(txn_id, write_record->GetTableName(), write_record->GetRid(), error.what());
            std::cerr << undo_error.what() << std::endl;
            undo_failed = true;
        } catch (...) {
            UndoError undo_error(txn_id, write_record->GetTableName(), write_record->GetRid(), "unknown exception");
            std::cerr << undo_error.what() << std::endl;
            undo_failed = true;
        }
    }

    if (log_manager != nullptr) {
        AbortLogRecord abort_log(txn_id);
        abort_log.prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = log_manager->add_log_to_buffer(&abort_log);
        txn->set_prev_lsn(lsn);
        log_manager->flush_log_to_disk();
    }

    txn->set_state(TransactionState::SHRINKING);
    release_locks(txn, lock_manager_);
    delete_write_set(txn);
    txn->set_state(TransactionState::ABORTED);

    std::vector<std::unique_ptr<Transaction>> released_txns;
    {
        std::unique_lock<std::shared_mutex> txn_lock(txn_map_mutex_);
        RemoveTransactionLocked(txn_id, &released_txns);
    }
}

lsn_t TransactionManager::create_static_checkpoint(LogManager *log_manager) {
    std::unique_lock<std::shared_mutex> checkpoint_lock(checkpoint_mutex_);
    if (log_manager == nullptr || sm_manager_ == nullptr) {
        return INVALID_LSN;
    }

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

    CheckpointLogRecord checkpoint(active_txns);
    LogAppendResult checkpoint_append = log_manager->add_log_to_buffer_with_result(&checkpoint);
    if (!checkpoint_append.IsValid()) {
        throw InternalError("checkpoint log record is too large");
    }
    log_manager->flush_log_to_disk();

    if (!sm_manager_->flush_storage()) {
        throw InternalError("checkpoint storage flush failed");
    }

    write_restart_file(RestartRecord{checkpoint_append.lsn, checkpoint_append.offset});

    return checkpoint_append.lsn;
}
