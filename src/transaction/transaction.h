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
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "common/common.h"
#include "transaction/txn_defs.h"
#include "record/rm_defs.h"

/** 表示此tuple的前一个版本的链接 */
struct UndoLink {
  /* 之前的版本可以在其中的事务中找到 */
  txn_id_t prev_txn_{INVALID_TXN_ID};
  /* 在 `prev_txn_` 中前一个版本的日志索引 */
  int prev_log_idx_{0};

  friend auto operator==(const UndoLink &a, const UndoLink &b) {
    return a.prev_txn_ == b.prev_txn_ && a.prev_log_idx_ == b.prev_log_idx_;
  }

  friend auto operator!=(const UndoLink &a, const UndoLink &b) { return !(a == b); }

  /* Checks if the undo link points to something. */
  bool IsValid() { return prev_txn_ != INVALID_TXN_ID; }
};

struct UndoLog {
  UndoLog() = default;

  UndoLog(const UndoLog &other)
      : is_deleted_(other.is_deleted_),
        old_record_(other.old_record_ == nullptr ? nullptr : std::make_unique<RmRecord>(*other.old_record_)),
        ts_(other.ts_),
        prev_version_(other.prev_version_) {}

  auto operator=(const UndoLog &other) -> UndoLog & {
    if (this == &other) {
      return *this;
    }
    is_deleted_ = other.is_deleted_;
    old_record_ = other.old_record_ == nullptr ? nullptr : std::make_unique<RmRecord>(*other.old_record_);
    ts_ = other.ts_;
    prev_version_ = other.prev_version_;
    return *this;
  }

  UndoLog(UndoLog &&other) noexcept = default;
  auto operator=(UndoLog &&other) noexcept -> UndoLog & = default;

  /* 此日志是否为删除标记 */
  bool is_deleted_;
  /* 旧版本记录的物理拷贝 */
  std::unique_ptr<RmRecord> old_record_;
  /* 此撤销日志的时间戳 */
  timestamp_t ts_{INVALID_TS};
  /* 撤销日志的前一个版本 */
  UndoLink prev_version_{};
};

/**
 * 记录一次 SSI 事务读取的记录，对rid点查的精确记录
 */
struct SsiRecordRead {
  std::string table_name_;
  Rid rid_;

  friend auto operator<(const SsiRecordRead &lhs, const SsiRecordRead &rhs) -> bool {
    return std::make_tuple(lhs.table_name_, lhs.rid_.page_no, lhs.rid_.slot_no) <
           std::make_tuple(rhs.table_name_, rhs.rid_.page_no, rhs.rid_.slot_no);
  }
};

/**
 * 记录一次 SSI 事务读取的谓词条件，用于判断是否满足读取条件
 */
struct SsiPredicateRead {
  std::string table_name_;
  std::vector<Condition> conditions_;
};

/**
 * 记录一次 SSI 事务写入的记录
 */
struct SsiWriteRecord {
  std::string table_name_;
  Rid rid_;
  RmRecord old_record_;
  RmRecord new_record_;
  bool has_old_record_{true};
  bool has_new_record_{true};
};

/**
 * 记录一次 SSI 事务依赖边，用于表示事务之间的依赖关系
 */
struct SsiDependencyEdge {
  txn_id_t other_txn_{INVALID_TXN_ID};
  std::string reason_;

  friend auto operator<(const SsiDependencyEdge &lhs, const SsiDependencyEdge &rhs) -> bool {
    return std::make_pair(lhs.other_txn_, lhs.reason_) < std::make_pair(rhs.other_txn_, rhs.reason_);
  }
};


class Transaction: public NonCopyable {
   public:
    explicit Transaction(txn_id_t txn_id, IsolationLevel isolation_level = IsolationLevel::SERIALIZABLE)
        : state_(TransactionState::DEFAULT), isolation_level_(isolation_level), txn_id_(txn_id) {
        write_set_ = std::make_shared<std::deque<WriteRecord *>>();
        lock_set_ = std::make_shared<std::unordered_set<LockDataId>>();
        index_latch_page_set_ = std::make_shared<std::deque<Page *>>();
        index_deleted_page_set_ = std::make_shared<std::deque<Page*>>();
        prev_lsn_ = INVALID_LSN;
        thread_id_ = std::this_thread::get_id();
    }

    ~Transaction() = default;

    inline txn_id_t get_transaction_id() { return txn_id_; }

    inline std::thread::id get_thread_id() { return thread_id_; }

    inline void set_txn_mode(bool txn_mode) { txn_mode_ = txn_mode; }
    inline bool get_txn_mode() { return txn_mode_; }

    inline void set_start_ts(timestamp_t start_ts) { start_ts_ = start_ts; }
    inline timestamp_t get_start_ts() { return start_ts_; }

    inline void set_commit_ts(timestamp_t commit_ts) { commit_ts_.store(commit_ts); }

    inline IsolationLevel get_isolation_level() { return isolation_level_; }
    inline void set_isolation_level(IsolationLevel level) { isolation_level_ = level; }

    inline TransactionState get_state() { return state_; }
    inline void set_state(TransactionState state) { state_ = state; }

    inline lsn_t get_prev_lsn() { return prev_lsn_; }
    inline void set_prev_lsn(lsn_t prev_lsn) { prev_lsn_ = prev_lsn; }

    inline std::shared_ptr<std::deque<WriteRecord *>> get_write_set() { return write_set_; }
    inline void append_write_record(WriteRecord* write_record) { write_set_->push_back(write_record); }

    inline std::shared_ptr<std::deque<Page*>> get_index_deleted_page_set() { return index_deleted_page_set_; }
    inline void append_index_deleted_page(Page* page) { index_deleted_page_set_->push_back(page); }

    inline std::shared_ptr<std::deque<Page*>> get_index_latch_page_set() { return index_latch_page_set_; }
    inline void append_index_latch_page_set(Page* page) { index_latch_page_set_->push_back(page); }

    inline std::shared_ptr<std::unordered_set<LockDataId>> get_lock_set() { return lock_set_; }

    inline timestamp_t get_commit_ts() const { return commit_ts_; }

    /** 修改现有的撤销日志 */
    inline auto ModifyUndoLog(int log_idx, UndoLog new_log) {
        std::scoped_lock<std::mutex> lck(latch_);
        undo_logs_[log_idx] = std::move(new_log);
      }

    /** @return 此事务中撤销日志的索引 */
    inline auto AppendUndoLog(UndoLog log) -> UndoLink {
        std::scoped_lock<std::mutex> lck(latch_);
        undo_logs_.emplace_back(std::move(log));
        return {txn_id_, static_cast<int>(undo_logs_.size() - 1)};
      }
    inline auto GetUndoLog(size_t log_id) -> UndoLog {
        std::scoped_lock<std::mutex> lck(latch_);
        return undo_logs_[log_id];
      }

    /** @return 撤销日志的数量 */
    inline auto GetUndoLogNum() -> size_t {
        std::scoped_lock<std::mutex> lck(latch_);
        return undo_logs_.size();
      }

    inline void AddRecordRead(const std::string &table_name, const Rid &rid) {
        std::scoped_lock<std::mutex> lck(latch_);
        record_read_set_.insert(SsiRecordRead{table_name, rid});
      }

    inline std::set<SsiRecordRead> GetRecordReadSet() const {
        std::scoped_lock<std::mutex> lck(latch_);
        return record_read_set_;
      }

    inline void AddPredicateRead(const std::string &table_name, std::vector<Condition> conditions) {
        std::scoped_lock<std::mutex> lck(latch_);
        sort_conditions(conditions);
        predicate_read_set_.push_back(SsiPredicateRead{table_name, std::move(conditions)});
      }

    inline std::vector<SsiPredicateRead> GetPredicateReadSet() const {
        std::scoped_lock<std::mutex> lck(latch_);
        return predicate_read_set_;
      }

    inline void AddSsiWriteSetRecord(const std::string &table_name, const Rid &rid, const RmRecord &old_record,
                                  const RmRecord &new_record, bool has_old_record = true,
                                  bool has_new_record = true) {
        std::scoped_lock<std::mutex> lck(latch_);
        write_set_records_.push_back(
            SsiWriteRecord{table_name, rid, old_record, new_record, has_old_record, has_new_record});
      }

    inline std::vector<SsiWriteRecord> GetWriteSetRecords() const {
        std::scoped_lock<std::mutex> lck(latch_);
        return write_set_records_;
      }
      
    inline bool AddRwDependencyIn(txn_id_t other_txn, std::string reason) {
        std::scoped_lock<std::mutex> lck(latch_);
        return in_edges_[other_txn].insert(SsiDependencyEdge{other_txn, std::move(reason)}).second;
      }

    inline bool AddRwDependencyOut(txn_id_t other_txn, std::string reason) {
        std::scoped_lock<std::mutex> lck(latch_);
        return out_edges_[other_txn].insert(SsiDependencyEdge{other_txn, std::move(reason)}).second;
      }

    inline std::map<txn_id_t, std::set<SsiDependencyEdge>> GetRwInEdges() const {
        std::scoped_lock<std::mutex> lck(latch_);
        return in_edges_;
      }

    inline std::map<txn_id_t, std::set<SsiDependencyEdge>> GetRwOutEdges() const {
        std::scoped_lock<std::mutex> lck(latch_);
        return out_edges_;
      }

    inline void RemoveRwDependencyIn(txn_id_t other_txn) {
        std::scoped_lock<std::mutex> lck(latch_);
        in_edges_.erase(other_txn);
      }

    inline void RemoveRwDependencyOut(txn_id_t other_txn) {
        std::scoped_lock<std::mutex> lck(latch_);
        out_edges_.erase(other_txn);
      }

    inline void ClearSsiState() {
        std::scoped_lock<std::mutex> lck(latch_);
        record_read_set_.clear();
        predicate_read_set_.clear();
        write_set_records_.clear();
        in_edges_.clear();
        out_edges_.clear();
      }


   private:
    bool txn_mode_;                   // 用于标识当前事务为显式事务还是单条SQL语句的隐式事务
    TransactionState state_;          // 事务状态
    IsolationLevel isolation_level_;  // 事务的隔离级别，默认隔离级别为可串行化
    std::thread::id thread_id_;       // 当前事务对应的线程id
    lsn_t prev_lsn_;                  // 当前事务执行的最后一条操作对应的lsn，用于系统故障恢复
    txn_id_t txn_id_;                 // 事务的ID，唯一标识符
    timestamp_t start_ts_;            // 事务的开始时间戳

    std::shared_ptr<std::deque<WriteRecord *>> write_set_;  // 事务包含的所有写操作
    std::shared_ptr<std::unordered_set<LockDataId>> lock_set_;  // 事务申请的所有锁
    std::shared_ptr<std::deque<Page*>> index_latch_page_set_;          // 维护事务执行过程中加锁的索引页面
    std::shared_ptr<std::deque<Page*>> index_deleted_page_set_;    // 维护事务执行过程中删除的索引页面

  /** 提交时间戳 */
  std::atomic<timestamp_t> commit_ts_{INVALID_TS};
  /**
  * @brief 存储撤销日志。
  * 其他撤销日志/表堆将存储 (txn_id, index) 对，因此只能向此vector中追加内容或就地更新内容，而不能删除任何内容。
  */
  std::vector<UndoLog> undo_logs_;
  std::set<SsiRecordRead> record_read_set_;
  std::vector<SsiPredicateRead> predicate_read_set_;
  std::vector<SsiWriteRecord> write_set_records_;
  std::map<txn_id_t, std::set<SsiDependencyEdge>> in_edges_;    // 事务的入边，即依赖于当前事务的其他事务
  std::map<txn_id_t, std::set<SsiDependencyEdge>> out_edges_;   // 事务的出边，即当前事务依赖于其他事务
  /** 用于访问事务级撤销日志的锁。 */
  mutable std::mutex latch_;
};
