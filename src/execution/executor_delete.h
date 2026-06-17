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
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "executor_filter.h"
#include "index/ix.h"
#include "system/sm.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

class DeleteExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                   // 表的元数据
    RmFileHandle *fh_;              // 表的数据文件句柄
    std::string tab_name_;          // 表名称
    SmManager *sm_manager_;
    std::unique_ptr<AbstractExecutor> scan_executor_;  // 扫描子执行器，负责提供待删除记录与位置

    auto leaf_scan_type(AbstractExecutor *executor) const -> std::string {
        auto *current = executor;
        while (auto *filter = dynamic_cast<FilterExecutor *>(current)) {
            current = filter->child_executor();
        }
        return current == nullptr ? std::string() : current->getType();
    }

    /**
     * @brief Delete index entries for a single record across all indexes.
     */
    void delete_index_entries(const std::vector<IxIndexHandle *> &index_handles, const RmRecord &rec, const Rid &rid) {
        for (size_t i = 0; i < tab_.indexes.size(); ++i) {
            auto &index = tab_.indexes[i];
            auto key = std::make_unique<char[]>(index.col_tot_len);
            index.build_key(key.get(), rec.data);
            index_handles[i]->delete_entry(key.get(), rid, context_->txn_);
        }
    }

   public:
    DeleteExecutor(SmManager *sm_manager, const std::string &tab_name,
                   std::unique_ptr<AbstractExecutor> scan_executor, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        scan_executor_ = std::move(scan_executor);
        set_children({scan_executor_.get()});
        context_ = context;
        if (context_ != nullptr && context_->lock_mgr_ != nullptr) {
            context_->lock_mgr_->lock_IX_on_table(context_->txn_, fh_->GetFd());
        }
    }

    /**
     * @brief Execute hybrid delete consumption based on the child scan type.
     *
     * Sequential scans can delete rows as they are produced.
     * Index scans must be drained first so index mutations do not destabilize iteration.
     */
    std::unique_ptr<RmRecord> NextImpl() override {
        // Pre-compute index handles (loop-invariant across rows)
        std::vector<IxIndexHandle *> index_handles;
        index_handles.reserve(tab_.indexes.size());
        for (size_t i = 0; i < tab_.indexes.size(); ++i) {
            index_handles.push_back(sm_manager_->get_ih(tab_name_, tab_.indexes[i].cols));
        }
        auto scan_type = leaf_scan_type(scan_executor_.get());

        if (scan_type == "SeqScanExecutor") {
            // Sequential scans can stream records one by one because deleting the current row
            // does not invalidate the remaining table scan order.
            for (scan_executor_->beginTuple(); !scan_executor_->is_end(); scan_executor_->nextTuple()) {
                Rid rid = scan_executor_->rid();
                if (context_ != nullptr && context_->lock_mgr_ != nullptr) {
                    context_->lock_mgr_->lock_exclusive_on_record(context_->txn_, rid, fh_->GetFd());
                }
                auto rec = scan_executor_->Next();
                lsn_t op_lsn = INVALID_LSN;
                if (context_ != nullptr && context_->txn_ != nullptr && context_->log_mgr_ != nullptr) {
                    lsn_t prev_lsn = context_->txn_->get_prev_lsn();
                    DeleteLogRecord log_record(context_->txn_->get_transaction_id(), *rec, rid, tab_name_);
                    log_record.prev_lsn_ = prev_lsn;
                    op_lsn = context_->log_mgr_->add_log_to_buffer(&log_record);
                    context_->txn_->set_prev_lsn(op_lsn);
                    context_->txn_->append_write_record(
                        new WriteRecord(WType::DELETE_TUPLE, tab_name_, rid, *rec, prev_lsn));
                }
                delete_index_entries(index_handles, *rec, rid);
                fh_->delete_record(rid, context_, op_lsn);
            }
            return nullptr;
        }

        if (scan_type == "IndexScanExecutor") {
            struct DeleteCandidate {
                Rid rid;
                std::unique_ptr<RmRecord> rec;
            };

            std::vector<DeleteCandidate> candidates;
            // Index scans must consume all visible tuples before any delete mutates index state,
            // otherwise the underlying scan cursor may observe an unstable key space.
            for (scan_executor_->beginTuple(); !scan_executor_->is_end(); scan_executor_->nextTuple()) {
                candidates.push_back(DeleteCandidate{scan_executor_->rid(), scan_executor_->Next()});
            }

            for (auto &candidate : candidates) {
                if (context_ != nullptr && context_->lock_mgr_ != nullptr) {
                    context_->lock_mgr_->lock_exclusive_on_record(context_->txn_, candidate.rid, fh_->GetFd());
                }
                lsn_t op_lsn = INVALID_LSN;
                if (context_ != nullptr && context_->txn_ != nullptr && context_->log_mgr_ != nullptr) {
                    lsn_t prev_lsn = context_->txn_->get_prev_lsn();
                    DeleteLogRecord log_record(context_->txn_->get_transaction_id(), *candidate.rec, candidate.rid,
                                               tab_name_);
                    log_record.prev_lsn_ = prev_lsn;
                    op_lsn = context_->log_mgr_->add_log_to_buffer(&log_record);
                    context_->txn_->set_prev_lsn(op_lsn);
                    context_->txn_->append_write_record(
                        new WriteRecord(WType::DELETE_TUPLE, tab_name_, candidate.rid, *candidate.rec, prev_lsn));
                }
                delete_index_entries(index_handles, *candidate.rec, candidate.rid);
                fh_->delete_record(candidate.rid, context_, op_lsn);
            }
            return nullptr;
        }

        throw InternalError("DeleteExecutor requires a scan executor child");
    }

    Rid &rid() override { return _abstract_rid; }
};
