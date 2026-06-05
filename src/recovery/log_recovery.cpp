/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "log_recovery.h"

#include <algorithm>
#include <fstream>
#include <queue>
#include <vector>


RmFileHandle *table_handle(SmManager *sm_manager, const char *name, size_t name_size) {
    std::string tab_name(name, name_size);
    auto it = sm_manager->fhs_.find(tab_name);
    if (it == sm_manager->fhs_.end()) {
        return nullptr;
    }
    return it->second.get();
}

using LogOffsetIndex = std::unordered_map<lsn_t, int>;

struct LogEntryHeader {
    LogRecord record;
    int offset = 0;
    uint32_t total_len = 0;
};

bool read_log_header(DiskManager *disk_manager, LogBuffer &buffer, int offset, int log_size,
                     LogEntryHeader &entry) {
    int bytes = disk_manager->read_log(buffer.buffer_, LOG_HEADER_SIZE, offset);
    if (bytes < LOG_HEADER_SIZE) {
        return false;
    }

    entry.record.deserialize(buffer.buffer_);
    entry.offset = offset;
    entry.total_len = entry.record.log_tot_len_;
    return entry.total_len != 0
        && entry.total_len <= LOG_BUFFER_SIZE
        && offset + static_cast<int>(entry.total_len) <= log_size;
}

/**
 * @brief: for each header do callback
 */
template <typename Callback>
void for_each_log_header_from(DiskManager *disk_manager, LogBuffer &buffer, int start_offset, Callback callback) {
    int log_size = disk_manager->get_file_size(LOG_FILE_NAME);
    int offset = start_offset;
    while (offset < log_size) {
        LogEntryHeader entry;
        if (!read_log_header(disk_manager, buffer, offset, log_size, entry)) {
            break;
        }
        if (!callback(entry)) {
            break;
        }
        offset += static_cast<int>(entry.total_len);
    }
}

template <typename Callback>
void for_each_log_header(DiskManager *disk_manager, LogBuffer &buffer, Callback callback) {
    for_each_log_header_from(disk_manager, buffer, 0, std::move(callback));
}

template <typename Callback>
void for_each_log_record_from(DiskManager *disk_manager, LogBuffer &buffer, int start_offset, Callback callback) {
    for_each_log_header_from(disk_manager, buffer, start_offset, [&](const LogEntryHeader &entry) {
        disk_manager->read_log(buffer.buffer_, entry.total_len, entry.offset);
        return callback(entry.record);
    });
}

template <typename Callback>
void for_each_log_record(DiskManager *disk_manager, LogBuffer &buffer, Callback callback) {
    for_each_log_record_from(disk_manager, buffer, 0, std::move(callback));
}

LogOffsetIndex build_log_offset_index(DiskManager *disk_manager, LogBuffer &buffer) {
    LogOffsetIndex offsets;
    for_each_log_header(disk_manager, buffer, [&](const LogEntryHeader &entry) {
        offsets[entry.record.lsn_] = entry.offset;
        return true;
    });
    return offsets;
}

int read_log_at_lsn(DiskManager *disk_manager, LogBuffer &buffer, const LogOffsetIndex &offsets, lsn_t target_lsn) {
    auto offset_it = offsets.find(target_lsn);
    if (offset_it == offsets.end()) {
        return -1;
    }

    int offset = offset_it->second;
    int log_size = disk_manager->get_file_size(LOG_FILE_NAME);
    LogEntryHeader entry;
    if (!read_log_header(disk_manager, buffer, offset, log_size, entry) || entry.record.lsn_ != target_lsn) {
        return -1;
    }

    disk_manager->read_log(buffer.buffer_, entry.total_len, offset);
    return offset;
}

bool index_has_rid(IxIndexHandle *index_handle, const char *key, const Rid &rid) {
    std::vector<Rid> result;
    if (!index_handle->get_value(key, &result, nullptr)) {
        return false;
    }
    for (const auto &entry : result) {
        if (entry == rid) {
            return true;
        }
    }
    return false;
}

void insert_index_entries(SmManager *sm_manager, const std::string &tab_name, const RmRecord &record,
                          const Rid &rid) {
    TabMeta &tab = sm_manager->db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        auto ih = sm_manager->get_ih(tab_name, index.cols);
        auto key = std::make_unique<char[]>(index.col_tot_len);
        index.build_key(key.get(), record.data);
        if (!index_has_rid(ih, key.get(), rid)) {
            ih->insert_entry(key.get(), rid, nullptr);
        }
    }
}

void delete_index_entries(SmManager *sm_manager, const std::string &tab_name, const RmRecord &record,
                          const Rid &rid) {
    TabMeta &tab = sm_manager->db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        auto ih = sm_manager->get_ih(tab_name, index.cols);
        auto key = std::make_unique<char[]>(index.col_tot_len);
        index.build_key(key.get(), record.data);
        ih->delete_entry(key.get(), rid, nullptr);
    }
}

void update_index_entries(SmManager *sm_manager, const std::string &tab_name, const RmRecord &old_record,
                          const RmRecord &new_record, const Rid &rid) {
    delete_index_entries(sm_manager, tab_name, old_record, rid);
    insert_index_entries(sm_manager, tab_name, new_record, rid);
}

lsn_t next_lsn_after_log(DiskManager *disk_manager, LogBuffer &buffer) {
    lsn_t max_lsn = INVALID_LSN;
    for_each_log_header(disk_manager, buffer, [&](const LogEntryHeader &entry) {
        max_lsn = std::max(max_lsn, entry.record.lsn_);
        return true;
    });
    return max_lsn + 1;
}

int offset_after_entry(const LogEntryHeader &entry) {
    return entry.offset + static_cast<int>(entry.total_len);
}

bool read_checkpoint_at_offset(DiskManager *disk_manager, LogBuffer &buffer, lsn_t checkpoint_lsn,
                               int checkpoint_lsn_offset, LogEntryHeader &entry) {
    int log_size = disk_manager->get_file_size(LOG_FILE_NAME);
    if (checkpoint_lsn_offset < 0 || checkpoint_lsn_offset >= log_size) {
        return false;
    }
    if (!read_log_header(disk_manager, buffer, checkpoint_lsn_offset, log_size, entry)) {
        return false;
    }
    return entry.record.log_type_ == LogType::CHECKPOINT && entry.record.lsn_ == checkpoint_lsn;
}

int resolve_checkpoint_offset(DiskManager *disk_manager, LogBuffer &buffer, lsn_t checkpoint_lsn,
                              int hinted_offset) {
    LogEntryHeader hinted_entry;
    if (read_checkpoint_at_offset(disk_manager, buffer, checkpoint_lsn, hinted_offset, hinted_entry)) {
        return hinted_entry.offset;
    }

    int checkpoint_offset = -1;
    for_each_log_header(disk_manager, buffer, [&](const LogEntryHeader &entry) {
        if (entry.record.lsn_ != checkpoint_lsn) {
            return true;
        }
        if (entry.record.log_type_ == LogType::CHECKPOINT) {
            checkpoint_offset = entry.offset;
        }
        return false;
    });
    return checkpoint_offset;
}

/**
 * @brief: from restart file decide offset for scanning wal
 */
bool find_checkpoint_from_restart(DiskManager *disk_manager, LogBuffer &buffer, int &checkpoint_offset,
                                  int &scan_start_offset) {
    checkpoint_offset = -1;
    scan_start_offset = 0;

    std::ifstream restart(DB_RESTART_NAME, std::ios::binary);
    if (!restart.is_open()) {
        return false;
    }
    lsn_t checkpoint_lsn = INVALID_LSN;
    restart.read(reinterpret_cast<char *>(&checkpoint_lsn), sizeof(checkpoint_lsn));
    if (restart.gcount() != static_cast<std::streamsize>(sizeof(checkpoint_lsn))) {
        return false;
    }

    if (checkpoint_lsn == INVALID_LSN) {
        return false;
    }

    int hinted_offset = -1;
    restart.read(reinterpret_cast<char *>(&hinted_offset), sizeof(hinted_offset));
    if (restart.gcount() != 0 && restart.gcount() != static_cast<std::streamsize>(sizeof(hinted_offset))) {
        return false;
    }

    int checkpoint_lsn_offset = resolve_checkpoint_offset(disk_manager, buffer, checkpoint_lsn, hinted_offset);
    if (checkpoint_lsn_offset < 0) {
        return false;
    }

    LogEntryHeader entry;
    int log_size = disk_manager->get_file_size(LOG_FILE_NAME);
    if (!read_log_header(disk_manager, buffer, checkpoint_lsn_offset, log_size, entry)) {
        return false;
    }
    if (entry.record.log_type_ != LogType::CHECKPOINT || entry.record.lsn_ != checkpoint_lsn) {
        return false;
    }
    checkpoint_offset = entry.offset;
    scan_start_offset = offset_after_entry(entry);
    return checkpoint_offset >= 0 && scan_start_offset <= log_size;
}

lsn_t append_recovery_log(DiskManager *disk_manager, LogRecord &log_record, lsn_t next_lsn) {
    log_record.lsn_ = next_lsn;
    std::vector<char> data(log_record.log_tot_len_);
    log_record.serialize(data.data());
    disk_manager->write_log(data.data(), static_cast<int>(data.size()));
    return log_record.lsn_;
}

lsn_t append_tracked_recovery_log(DiskManager *disk_manager, LogRecord &log_record, lsn_t &next_lsn,
                                  LogOffsetIndex &log_offsets,
                                  std::unordered_map<txn_id_t, RecoveredTxnEntry> &txn_table) {
    lsn_t appended_lsn = append_recovery_log(disk_manager, log_record, next_lsn++);
    log_offsets[appended_lsn] = disk_manager->get_file_size(LOG_FILE_NAME) -
                                static_cast<int>(log_record.log_tot_len_);
    txn_table[log_record.log_tid_].last_lsn = appended_lsn;
    return appended_lsn;
}

template <typename ClrRecord>
lsn_t append_clr(DiskManager *disk_manager, ClrRecord &clr, LogType clr_type, lsn_t undo_next_lsn, lsn_t &next_lsn,
                 LogOffsetIndex &log_offsets,
                 std::unordered_map<txn_id_t, RecoveredTxnEntry> &txn_table) {
    clr.log_type_ = clr_type;
    auto txn_it = txn_table.find(clr.log_tid_);
    clr.prev_lsn_ = txn_it != txn_table.end() ? txn_it->second.last_lsn : INVALID_LSN;
    clr.undo_next_lsn_ = undo_next_lsn;
    return append_tracked_recovery_log(disk_manager, clr, next_lsn, log_offsets, txn_table);
}

template <typename LogRecordT, typename PrepareFn, typename ApplyFn>
void redo_dml(SmManager *sm_manager, const LogRecordT &log_record, PrepareFn prepare, ApplyFn apply) {
    std::string tab_name(log_record.table_name_, log_record.table_name_size_);
    RmFileHandle *fh = table_handle(sm_manager, log_record.table_name_, log_record.table_name_size_);
    if (fh == nullptr || !prepare(fh, log_record.rid_)) {
        return;
    }
    if (fh->get_page_lsn(log_record.rid_) >= log_record.lsn_) {
        return;
    }
    apply(fh, tab_name);
    fh->set_page_lsn(log_record.rid_, log_record.lsn_);
}

void RecoveryManager::analyze() {
    txn_table_.clear();
    checkpoint_record_offset_ = -1;
    scan_start_offset_ = 0;

    // set the offset according to checkpoint and build txn_table_
    if (find_checkpoint_from_restart(disk_manager_, buffer_, checkpoint_record_offset_, scan_start_offset_)) {
        int log_size = disk_manager_->get_file_size(LOG_FILE_NAME);
        LogEntryHeader checkpoint_entry;
        if (read_log_header(disk_manager_, buffer_, checkpoint_record_offset_, log_size, checkpoint_entry)) {
            disk_manager_->read_log(buffer_.buffer_, checkpoint_entry.total_len, checkpoint_record_offset_);
            CheckpointLogRecord checkpoint;
            checkpoint.deserialize(buffer_.buffer_);
            for (const auto &entry : checkpoint.active_txns_) {
                txn_table_[entry.txn_id] = RecoveredTxnEntry{RecoveredTxnState::RUNNING, entry.last_lsn};
            }
        }
    }

    // mark following txn_table_
    for_each_log_header_from(disk_manager_, buffer_, scan_start_offset_, [&](const LogEntryHeader &entry) {
        const LogRecord &header = entry.record;
        if (header.log_tid_ != INVALID_TXN_ID) {
            txn_table_[header.log_tid_].last_lsn = header.lsn_;
        }
        if (header.log_type_ == LogType::begin) {
            txn_table_[header.log_tid_].state = RecoveredTxnState::RUNNING;
        } else if (header.log_type_ == LogType::commit) {
            txn_table_[header.log_tid_].state = RecoveredTxnState::COMMITTED;
        } else if (header.log_type_ == LogType::ABORT) {
            txn_table_[header.log_tid_].state = RecoveredTxnState::ABORTED;
        }
        return true;
    });
}

void RecoveryManager::redo() {
    for_each_log_record_from(disk_manager_, buffer_, scan_start_offset_, [&](const LogRecord &header) {
        if (header.log_type_ == LogType::INSERT || header.log_type_ == LogType::CLR_INSERT) {
            InsertLogRecord log_record;
            log_record.deserialize(buffer_.buffer_);
            redo_dml(sm_manager_, log_record,
                     [](RmFileHandle *fh, const Rid &rid) {
                         fh->ensure_page_exists(rid.page_no);
                         return true;
                     },
                     [&](RmFileHandle *fh, const std::string &tab_name) {
                         fh->insert_record(log_record.rid_, log_record.insert_value_.data);
                         insert_index_entries(sm_manager_, tab_name, log_record.insert_value_, log_record.rid_);
                     });
        } else if (header.log_type_ == LogType::DELETE || header.log_type_ == LogType::CLR_DELETE) {
            DeleteLogRecord log_record;
            log_record.deserialize(buffer_.buffer_);
            redo_dml(sm_manager_, log_record,
                     [](RmFileHandle *fh, const Rid &rid) { return rid.page_no < fh->get_file_hdr().num_pages; },
                     [&](RmFileHandle *fh, const std::string &tab_name) {
                         delete_index_entries(sm_manager_, tab_name, log_record.delete_value_, log_record.rid_);
                         fh->delete_record(log_record.rid_, nullptr);
                     });
        } else if (header.log_type_ == LogType::UPDATE || header.log_type_ == LogType::CLR_UPDATE) {
            UpdateLogRecord log_record;
            log_record.deserialize(buffer_.buffer_);
            redo_dml(sm_manager_, log_record,
                     [](RmFileHandle *fh, const Rid &rid) { return rid.page_no < fh->get_file_hdr().num_pages; },
                     [&](RmFileHandle *fh, const std::string &tab_name) {
                         update_index_entries(sm_manager_, tab_name, log_record.old_value_, log_record.new_value_,
                                              log_record.rid_);
                         fh->update_record(log_record.rid_, log_record.new_value_.data, nullptr);
                     });
        }
        return true;
    });
}

void RecoveryManager::undo() {
    std::priority_queue<std::pair<lsn_t, txn_id_t>> undo_queue;
    for (const auto &[txn_id, entry] : txn_table_) {
        if (entry.state == RecoveredTxnState::RUNNING && entry.last_lsn != INVALID_LSN) {
            undo_queue.emplace(entry.last_lsn, txn_id);
        }
    }
    if (undo_queue.empty()) {
        return;
    }

    lsn_t next_lsn = next_lsn_after_log(disk_manager_, buffer_);
    LogOffsetIndex log_offsets = build_log_offset_index(disk_manager_, buffer_);

    while (!undo_queue.empty()) {
        auto [current_lsn, txn_id] = undo_queue.top();
        undo_queue.pop();

        if (read_log_at_lsn(disk_manager_, buffer_, log_offsets, current_lsn) < 0) {
            continue;
        }

        LogRecord header;
        header.deserialize(buffer_.buffer_);
        lsn_t next_undo_lsn = INVALID_LSN;

        if (header.log_type_ == LogType::CLR_INSERT || header.log_type_ == LogType::CLR_DELETE ||
            header.log_type_ == LogType::CLR_UPDATE) {
            next_undo_lsn = header.undo_next_lsn_;
        } else if (header.log_type_ == LogType::begin) {
            AbortLogRecord abort_log(txn_id);
            auto txn_it = txn_table_.find(txn_id);
            abort_log.prev_lsn_ = txn_it != txn_table_.end() ? txn_it->second.last_lsn : INVALID_LSN;
            append_tracked_recovery_log(disk_manager_, abort_log, next_lsn, log_offsets, txn_table_);
            txn_table_[txn_id].state = RecoveredTxnState::ABORTED;
            continue;
        } else if (header.log_type_ == LogType::INSERT) {
            InsertLogRecord log_record;
            log_record.deserialize(buffer_.buffer_);
            std::string tab_name(log_record.table_name_, log_record.table_name_size_);
            RmFileHandle *fh = table_handle(sm_manager_, log_record.table_name_, log_record.table_name_size_);
            if (fh != nullptr) {
                auto rec = fh->get_record(log_record.rid_, nullptr);
                DeleteLogRecord clr(txn_id, *rec, log_record.rid_, tab_name);
                lsn_t clr_lsn = append_clr(disk_manager_, clr, LogType::CLR_DELETE, header.prev_lsn_, next_lsn,
                                           log_offsets, txn_table_);
                delete_index_entries(sm_manager_, tab_name, *rec, log_record.rid_);
                fh->delete_record(log_record.rid_, nullptr, clr_lsn);
            }
            next_undo_lsn = header.prev_lsn_;
        } else if (header.log_type_ == LogType::DELETE) {
            DeleteLogRecord log_record;
            log_record.deserialize(buffer_.buffer_);
            std::string tab_name(log_record.table_name_, log_record.table_name_size_);
            RmFileHandle *fh = table_handle(sm_manager_, log_record.table_name_, log_record.table_name_size_);
            if (fh != nullptr) {
                InsertLogRecord clr(txn_id, log_record.delete_value_, log_record.rid_, tab_name);
                lsn_t clr_lsn = append_clr(disk_manager_, clr, LogType::CLR_INSERT, header.prev_lsn_, next_lsn,
                                           log_offsets, txn_table_);
                fh->insert_record(log_record.rid_, log_record.delete_value_.data, clr_lsn);
                insert_index_entries(sm_manager_, tab_name, log_record.delete_value_, log_record.rid_);
            }
            next_undo_lsn = header.prev_lsn_;
        } else if (header.log_type_ == LogType::UPDATE) {
            UpdateLogRecord log_record;
            log_record.deserialize(buffer_.buffer_);
            std::string tab_name(log_record.table_name_, log_record.table_name_size_);
            RmFileHandle *fh = table_handle(sm_manager_, log_record.table_name_, log_record.table_name_size_);
            if (fh != nullptr) {
                UpdateLogRecord clr(txn_id, log_record.new_value_, log_record.old_value_, log_record.rid_, tab_name);
                lsn_t clr_lsn = append_clr(disk_manager_, clr, LogType::CLR_UPDATE, header.prev_lsn_, next_lsn,
                                           log_offsets, txn_table_);
                update_index_entries(sm_manager_, tab_name, log_record.new_value_, log_record.old_value_,
                                     log_record.rid_);
                fh->update_record(log_record.rid_, log_record.old_value_.data, nullptr, clr_lsn);
            }
            next_undo_lsn = header.prev_lsn_;
        }

        if (next_undo_lsn != INVALID_LSN) {
            undo_queue.emplace(next_undo_lsn, txn_id);
        }
    }
}
