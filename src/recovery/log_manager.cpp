/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <cstring>
#include "log_manager.h"

lsn_t next_lsn_from_disk(DiskManager *disk_manager) {
    int log_size = disk_manager->get_file_size(LOG_FILE_NAME);
    if (log_size <= 0) {
        return 0;
    }

    char header_buffer[LOG_HEADER_SIZE];
    lsn_t max_lsn = INVALID_LSN;
    int offset = 0;
    while (offset < log_size) {
        int bytes = disk_manager->read_log(header_buffer, LOG_HEADER_SIZE, offset);
        if (bytes < LOG_HEADER_SIZE) {
            break;
        }
        LogRecord header;
        header.deserialize(header_buffer);
        if (header.log_tot_len_ == 0 || offset + static_cast<int>(header.log_tot_len_) > log_size) {
            break;
        }
        max_lsn = std::max(max_lsn, header.lsn_);
        offset += static_cast<int>(header.log_tot_len_);
    }
    return max_lsn + 1;
}

LogManager::LogManager(DiskManager* disk_manager)
    : written_lsn_(INVALID_LSN), persist_lsn_(INVALID_LSN), disk_manager_(disk_manager) {
    sync_global_lsn_with_disk();
}

lsn_t LogManager::sync_global_lsn_with_disk() {
    lsn_t next_lsn = next_lsn_from_disk(disk_manager_);
    if (global_lsn_ < next_lsn) {
        global_lsn_ = next_lsn;
    }
    if (next_lsn > 0) {
        written_lsn_ = next_lsn - 1;
    }
    return next_lsn;
}

/**
 * @description: 添加日志记录到日志缓冲区中，并返回日志记录号
 * @param {LogRecord*} log_record 要写入缓冲区的日志记录
 * @return {lsn_t} 返回该日志的日志记录号
 */
lsn_t LogManager::add_log_to_buffer(LogRecord* log_record) {
    std::lock_guard<std::mutex> lock(latch_);
    if (log_record == nullptr) {
        return INVALID_LSN;
    }
    // 拒绝单条超过缓冲区大小的日志记录，防止溢出
    if (log_record->log_tot_len_ > LOG_BUFFER_SIZE) {
        return INVALID_LSN;
    }
    if (log_buffer_.is_full(log_record->log_tot_len_)) {
        flush_buffer_to_disk();
    }
    log_record->lsn_ = global_lsn_++;
    log_record->serialize(log_buffer_.buffer_ + log_buffer_.offset_);
    log_buffer_.offset_ += static_cast<int>(log_record->log_tot_len_);
    return log_record->lsn_;
}

void LogManager::flush_buffer_to_disk() {
    if (log_buffer_.offset_ == 0) {
        return;
    }
    disk_manager_->write_log(log_buffer_.buffer_, log_buffer_.offset_);
    written_lsn_ = global_lsn_ - 1;
    log_buffer_.offset_ = 0;
    memset(log_buffer_.buffer_, 0, sizeof(log_buffer_.buffer_));
}

void LogManager::sync_written_log() {
    disk_manager_->sync_log();
    persist_lsn_ = written_lsn_;
}

/**
 * @description: 把日志缓冲区的内容刷到磁盘中，由于目前只设置了一个缓冲区，因此需要阻塞其他日志操作
 */
void LogManager::flush_log_to_disk() {
    std::lock_guard<std::mutex> lock(latch_);
    if (log_buffer_.offset_ == 0) {
        return;
    }
    flush_buffer_to_disk();
    sync_written_log();
}

/**
 * @description: 把日志缓冲区的内容刷到磁盘中，直到达到目标lsn
 */
void LogManager::flush_log_to_lsn(lsn_t target_lsn) {
    if (target_lsn == INVALID_LSN) {
        return;
    }

    std::lock_guard<std::mutex> lock(latch_);
    if (persist_lsn_ >= target_lsn) {
        return;
    }
    if (written_lsn_ >= target_lsn) {
        sync_written_log();
        return;
    }
    if (log_buffer_.offset_ == 0) {
        return;
    }
    flush_buffer_to_disk();
    sync_written_log();
}
