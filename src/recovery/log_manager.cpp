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
#include <chrono>
#include <fstream>
#include "log_manager.h"

namespace {

bool read_restart_record(RestartRecord &record) {
    std::ifstream restart(DB_RESTART_NAME, std::ios::binary);
    if (!restart.is_open()) {
        return false;
    }

    restart.read(reinterpret_cast<char *>(&record.checkpoint_lsn), sizeof(record.checkpoint_lsn));
    if (restart.gcount() != static_cast<std::streamsize>(sizeof(record.checkpoint_lsn))) {
        return false;
    }
    restart.read(reinterpret_cast<char *>(&record.checkpoint_offset), sizeof(record.checkpoint_offset));
    if (restart.gcount() != 0 && restart.gcount() != static_cast<std::streamsize>(sizeof(record.checkpoint_offset))) {
        return false;
    }
    return record.checkpoint_lsn != INVALID_LSN;
}

lsn_t next_lsn_from_disk(DiskManager *disk_manager, int start_offset) {
    int log_size = disk_manager->get_file_size(LOG_FILE_NAME);
    if (log_size <= 0) {
        return 0;
    }

    char header_buffer[LOG_HEADER_SIZE];
    lsn_t max_lsn = INVALID_LSN;
    int offset = start_offset;
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

lsn_t next_lsn_from_checkpoint_tail(DiskManager *disk_manager) {
    RestartRecord restart_record;
    if (!read_restart_record(restart_record) || restart_record.checkpoint_offset < 0) {
        return INVALID_LSN;
    }

    int log_size = disk_manager->get_file_size(LOG_FILE_NAME);
    if (restart_record.checkpoint_offset >= log_size) {
        return INVALID_LSN;
    }

    char header_buffer[LOG_HEADER_SIZE];
    int bytes = disk_manager->read_log(header_buffer, LOG_HEADER_SIZE, restart_record.checkpoint_offset);
    if (bytes < LOG_HEADER_SIZE) {
        return INVALID_LSN;
    }

    LogRecord checkpoint_header;
    checkpoint_header.deserialize(header_buffer);
    if (checkpoint_header.log_type_ != LogType::CHECKPOINT ||
        checkpoint_header.lsn_ != restart_record.checkpoint_lsn ||
        checkpoint_header.log_tot_len_ == 0 ||
        restart_record.checkpoint_offset + static_cast<int>(checkpoint_header.log_tot_len_) > log_size) {
        return INVALID_LSN;
    }

    lsn_t tail_next_lsn = next_lsn_from_disk(disk_manager, restart_record.checkpoint_offset);
    return std::max(tail_next_lsn, restart_record.checkpoint_lsn + 1);
}

lsn_t next_lsn_from_disk(DiskManager *disk_manager) {
    lsn_t checkpoint_tail_next_lsn = next_lsn_from_checkpoint_tail(disk_manager);
    if (checkpoint_tail_next_lsn != INVALID_LSN) {
        return checkpoint_tail_next_lsn;
    }
    return next_lsn_from_disk(disk_manager, 0);
}

}  // namespace

LogManager::LogManager(DiskManager* disk_manager)
    : written_lsn_(INVALID_LSN), persist_lsn_(INVALID_LSN), disk_manager_(disk_manager) {
    sync_global_lsn_with_disk();
    flush_thread_ = std::thread(&LogManager::run_flush_thread, this);
}

LogManager::~LogManager() {
    stop_flag_.store(true, std::memory_order_release);
    flush_cv_.notify_all();
    if (flush_thread_.joinable()) {
        flush_thread_.join();
    }
    try {
        flush_log_to_disk();
    } catch (const std::exception &error) {
        std::cerr << "LogManager shutdown flush failed: " << error.what() << std::endl;
    }
}

lsn_t LogManager::sync_global_lsn_with_disk() {
    lsn_t next_lsn = next_lsn_from_disk(disk_manager_);
    if (global_lsn_.load(std::memory_order_acquire) < next_lsn) {
        global_lsn_.store(next_lsn, std::memory_order_release);
    }
    if (next_lsn > 0) {
        written_lsn_.store(next_lsn - 1, std::memory_order_release);
        persist_lsn_.store(next_lsn - 1, std::memory_order_release);
    }
    return next_lsn;
}

/**
 * @description: 添加日志记录到日志缓冲区中，并返回日志记录号
 * @param {LogRecord*} log_record 要写入缓冲区的日志记录
 * @return {lsn_t} 返回该日志的日志记录号
 */
lsn_t LogManager::add_log_to_buffer(LogRecord* log_record) {
    return add_log_to_buffer_with_result(log_record).lsn;
}

LogAppendResult LogManager::add_log_to_buffer_with_result(LogRecord *log_record) {
    std::unique_lock<std::mutex> lock(latch_);
    if (log_record == nullptr) {
        return {};
    }
    // 拒绝单条超过缓冲区大小的日志记录，防止溢出
    if (log_record->log_tot_len_ > LOG_BUFFER_SIZE) {
        return {};
    }
    if (log_buffer_.is_full(log_record->log_tot_len_)) {
        flush_buffer_to_disk();
        flush_cv_.notify_one();
    }
    LogAppendResult result;
    result.offset = static_cast<int>(disk_manager_->get_log_file_size()) + log_buffer_.offset_;
    log_record->lsn_ = global_lsn_.fetch_add(1, std::memory_order_acq_rel);
    result.lsn = log_record->lsn_;
    log_record->serialize(log_buffer_.buffer_ + log_buffer_.offset_);
    log_buffer_.offset_ += static_cast<int>(log_record->log_tot_len_);

    double fill_ratio = static_cast<double>(log_buffer_.offset_) / static_cast<double>(LOG_BUFFER_SIZE);
    if (fill_ratio >= FLUSH_HIGH_WATERMARK) {
        lsn_t wait_lsn = result.lsn;
        lock.unlock();
        flush_cv_.notify_one();
        std::unique_lock<std::mutex> cv_lock(cv_mutex_);
        flush_cv_.wait(cv_lock, [this, wait_lsn] {
            return persist_lsn_.load(std::memory_order_acquire) >= wait_lsn;
        });
    } else if (fill_ratio >= FLUSH_LOW_WATERMARK) {
        lock.unlock();
        flush_cv_.notify_one();
    }
    return result;
}

lsn_t LogManager::flush_buffer_to_disk() {
    if (log_buffer_.offset_ == 0) {
        return INVALID_LSN;
    }
    disk_manager_->write_log(log_buffer_.buffer_, log_buffer_.offset_);
    lsn_t target_lsn = global_lsn_.load(std::memory_order_acquire) - 1;
    written_lsn_.store(target_lsn, std::memory_order_release);
    log_buffer_.offset_ = 0;
    memset(log_buffer_.buffer_, 0, sizeof(log_buffer_.buffer_));
    return target_lsn;
}

void LogManager::advance_persist_lsn(lsn_t target_lsn) {
    lsn_t current = persist_lsn_.load(std::memory_order_acquire);
    while (target_lsn > current &&
           !persist_lsn_.compare_exchange_weak(current, target_lsn, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
    }
}

void LogManager::sync_written_log(lsn_t target_lsn) {
    if (target_lsn == INVALID_LSN) {
        return;
    }
    disk_manager_->sync_log();
    advance_persist_lsn(target_lsn);
    flush_cv_.notify_all();
}

void LogManager::run_flush_thread() {
    while (true) {
        {
            std::unique_lock<std::mutex> cv_lock(cv_mutex_);
            flush_cv_.wait_for(cv_lock, std::chrono::milliseconds(FLUSH_TIMEOUT_MS));
        }
        if (stop_flag_.load(std::memory_order_acquire)) {
            break;
        }

        lsn_t target_lsn = INVALID_LSN;
        {
            std::lock_guard<std::mutex> lock(latch_);
            target_lsn = flush_buffer_to_disk();
        }
        if (target_lsn != INVALID_LSN) {
            sync_written_log(target_lsn);
        }
    }
}

void LogManager::flush_buffer_to_disk_safe() {
    std::lock_guard<std::mutex> lock(latch_);
    flush_buffer_to_disk();
}

/**
 * @description: 把日志缓冲区的内容刷到磁盘中，由于目前只设置了一个缓冲区，因此需要阻塞其他日志操作
 */
void LogManager::flush_log_to_disk() {
    std::lock_guard<std::mutex> lock(latch_);
    lsn_t target_lsn = written_lsn_.load(std::memory_order_acquire);
    if (log_buffer_.offset_ > 0) {
        target_lsn = flush_buffer_to_disk();
    }
    if (target_lsn != INVALID_LSN &&
        persist_lsn_.load(std::memory_order_acquire) < target_lsn) {
        sync_written_log(target_lsn);
    }
}

/**
 * @description: 把日志缓冲区的内容刷到磁盘中，直到达到目标lsn
 */
void LogManager::flush_log_to_lsn(lsn_t target_lsn) {
    if (target_lsn == INVALID_LSN) {
        return;
    }

    if (persist_lsn_.load(std::memory_order_acquire) >= target_lsn) {
        return;
    }

    std::lock_guard<std::mutex> lock(latch_);
    if (persist_lsn_.load(std::memory_order_acquire) >= target_lsn) {
        return;
    }
    lsn_t written_lsn = written_lsn_.load(std::memory_order_acquire);
    if (written_lsn >= target_lsn) {
        sync_written_log(written_lsn);
        return;
    }
    if (log_buffer_.offset_ == 0) {
        return;
    }
    lsn_t flushed_lsn = flush_buffer_to_disk();
    sync_written_log(flushed_lsn);
}
