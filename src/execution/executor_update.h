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
#include "executor_index_scan.h"
#include "executor_seq_scan.h"
#include "index/ix.h"
#include "system/sm.h"
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    RmFileHandle *fh_;
    std::unique_ptr<AbstractExecutor> scan_executor_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

    auto leaf_scan_type(AbstractExecutor *executor) const -> std::string {
        auto *current = executor;
        while (auto *filter = dynamic_cast<FilterExecutor *>(current)) {
            current = filter->child_executor();
        }
        return current == nullptr ? std::string() : current->getType();
    }

    struct PreparedIndexState {
        std::vector<ColMeta *> set_cols;
        std::vector<IxIndexHandle *> index_handles;
        int record_size = 0;
    };

    struct BatchUniqueState {
        std::vector<std::vector<std::vector<Rid>>> ignored_rids_by_index_row;
    };

    /**
     * @brief Prepare metadata shared by both update execution paths.
     */
    auto prepare_update_state() -> PreparedIndexState {
        PreparedIndexState state;
        state.set_cols.reserve(set_clauses_.size());
        for (auto &set_clause : set_clauses_) {
            auto &col = *tab_.get_col(set_clause.lhs.col_name);
            set_clause.rhs = coerce_value_to_type(set_clause.rhs, col.type);
            if (set_clause.rhs.raw == nullptr) {
                set_clause.rhs.init_raw(col.len);
            }
            state.set_cols.push_back(&col);
        }

        state.record_size = fh_->get_file_hdr().record_size;
        state.index_handles.reserve(tab_.indexes.size());
        for (size_t i = 0; i < tab_.indexes.size(); ++i) {
            state.index_handles.push_back(sm_manager_->get_ih(tab_name_, tab_.indexes[i].cols));
        }
        return state;
    }

    /**
     * @brief Materialize the post-update record image from one scanned row.
     */
    auto build_updated_record(const PreparedIndexState &state, const RmRecord &old_rec) const -> std::unique_ptr<RmRecord> {
        auto new_rec = std::make_unique<RmRecord>(state.record_size);
        memcpy(new_rec->data, old_rec.data, state.record_size);
        for (size_t i = 0; i < set_clauses_.size(); ++i) {
            auto &col = *state.set_cols[i];
            memcpy(new_rec->data + col.offset, set_clauses_[i].rhs.raw->data, col.len);
        }
        return new_rec;
    }

    auto build_index_key(const IndexMeta &index, const RmRecord &record) const -> std::unique_ptr<char[]> {
        auto key = std::make_unique<char[]>(index.col_tot_len);
        index.build_key(key.get(), record.data);
        return key;
    }

    /**
     * @brief Apply index maintenance and record replacement for one row.
     */
    void apply_row_update(const PreparedIndexState &state, const Rid &rid, const RmRecord &old_rec,
                          const RmRecord &new_rec, const std::vector<std::vector<Rid>> *ignored_rids_by_index) {
        auto write_guard = context_ != nullptr && context_->txn_mgr_ != nullptr
                               ? context_->txn_mgr_->write_txn_guard()
                               : TransactionManager::WriteTxnGuard(nullptr);
        bool has_prior_writes = context_ != nullptr && context_->txn_ != nullptr &&
                                !context_->txn_->get_write_set()->empty();
        bool use_mvcc = is_mvcc_active(context_);
        if (!has_prior_writes) {
            check_write_conflict(context_, fh_->GetFd(), rid);
        }
        if (context_ != nullptr && context_->lock_mgr_ != nullptr) {
            context_->lock_mgr_->lock_exclusive_on_record(context_->txn_, rid, fh_->GetFd());
        }
        if (has_prior_writes) {
            check_write_conflict(context_, fh_->GetFd(), rid);
        }
        track_ssi_write(context_, tab_name_, rid, &old_rec, &new_rec);
        lsn_t op_lsn = INVALID_LSN;
        if (context_ != nullptr && context_->txn_ != nullptr && context_->log_mgr_ != nullptr) {
            lsn_t prev_lsn = context_->txn_->get_prev_lsn();
            UpdateLogRecord log_record(context_->txn_->get_transaction_id(), old_rec, new_rec, rid, tab_name_);
            log_record.prev_lsn_ = prev_lsn;
            op_lsn = context_->log_mgr_->add_log_to_buffer(&log_record);
            context_->txn_->set_prev_lsn(op_lsn);
            context_->txn_->append_write_record(new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rid, old_rec, prev_lsn));
        }
        if (context_ != nullptr && context_->txn_mgr_ != nullptr && context_->txn_ != nullptr) {
            context_->txn_mgr_->PrepareUpdate(fh_->GetFd(), rid, old_rec, context_->txn_);
        }
        fh_->update_record(rid, new_rec.data, context_);
        if (op_lsn != INVALID_LSN) {
            fh_->set_page_lsn(rid, op_lsn);
        }

        for (size_t i = 0; i < tab_.indexes.size(); ++i) {
            auto old_key = build_index_key(tab_.indexes[i], old_rec);
            auto new_key = build_index_key(tab_.indexes[i], new_rec);
            if (use_mvcc && memcmp(old_key.get(), new_key.get(), tab_.indexes[i].col_tot_len) == 0) {
                continue;
            }
            if (!use_mvcc) {
                state.index_handles[i]->delete_entry(old_key.get(), rid, context_->txn_);
            }
            const std::vector<Rid> *ignored_rids = nullptr;
            if (ignored_rids_by_index != nullptr && i < ignored_rids_by_index->size()) {
                ignored_rids = &(*ignored_rids_by_index)[i];
            }
            state.index_handles[i]->insert_entry(new_key.get(), rid, context_->txn_, ignored_rids);
        }
    }

    /**
     * @brief Preserve buffered unique-check semantics for index-scan children.
     */
    auto precheck_unique_for_batch(const PreparedIndexState &state, const std::vector<Rid> &rids,
                                   const std::vector<std::unique_ptr<RmRecord>> &old_records,
                                   const std::vector<std::unique_ptr<RmRecord>> &new_records) -> BatchUniqueState {
        BatchUniqueState batch_state;
        batch_state.ignored_rids_by_index_row.resize(rids.size(), std::vector<std::vector<Rid>>(tab_.indexes.size()));
        for (size_t i = 0; i < tab_.indexes.size(); ++i) {
            auto &index = tab_.indexes[i];
            if (!index.unique) {
                continue;
            }

            std::vector<std::string> old_key_bytes;
            std::vector<std::string> new_key_bytes;
            old_key_bytes.reserve(rids.size());
            new_key_bytes.reserve(rids.size());
            for (size_t row = 0; row < rids.size(); ++row) {
                auto old_key = build_index_key(index, *old_records[row]);
                auto new_key = build_index_key(index, *new_records[row]);
                old_key_bytes.emplace_back(old_key.get(), index.col_tot_len);
                new_key_bytes.emplace_back(new_key.get(), index.col_tot_len);
            }

            std::unordered_map<std::string, size_t> owner_by_new_key;
            owner_by_new_key.reserve(rids.size());
            for (size_t row = 0; row < rids.size(); ++row) {
                auto [owner_it, inserted] = owner_by_new_key.emplace(new_key_bytes[row], row);
                if (!inserted && rids[owner_it->second] != rids[row]) {
                    throw UniqueViolationError(tab_name_, index.col_names());
                }
                std::vector<Rid> ignored_rids;
                for (size_t other = 0; other < rids.size(); ++other) {
                    if (other == row) {
                        continue;
                    }
                    if (old_key_bytes[other] == new_key_bytes[row] &&
                        new_key_bytes[other] != new_key_bytes[row]) {
                        ignored_rids.push_back(rids[other]);
                    }
                }
                batch_state.ignored_rids_by_index_row[row][i] = ignored_rids;
                state.index_handles[i]->validate_unique_key(new_key_bytes[row].data(), context_->txn_, &rids[row],
                                                            &ignored_rids);
            }
        }
        return batch_state;
    }

    /**
     * @brief Drain the full scan result before mutating any index entry.
     */
    void execute_buffered_update_path(const PreparedIndexState &state) {
        std::vector<Rid> rids;
        std::vector<std::unique_ptr<RmRecord>> old_records;
        std::vector<std::unique_ptr<RmRecord>> new_records;

        scan_executor_->beginTuple();
        for (; !scan_executor_->is_end(); scan_executor_->nextTuple()) {
            rids.push_back(scan_executor_->rid());
            auto old_rec = scan_executor_->Next();
            new_records.push_back(build_updated_record(state, *old_rec));
            old_records.push_back(std::move(old_rec));
        }

        BatchUniqueState batch_state = precheck_unique_for_batch(state, rids, old_records, new_records);

        for (size_t row = 0; row < rids.size(); ++row) {
            apply_row_update(state, rids[row], *old_records[row], *new_records[row],
                             &batch_state.ignored_rids_by_index_row[row]);
        }
    }

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::unique_ptr<AbstractExecutor> scan_executor, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
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
     * @brief Execute hybrid update consumption based on the child scan type.
     *
     * Sequential scans persist row by row.
     * Index scans are fully consumed first to keep the scan cursor stable.
     */
    std::unique_ptr<RmRecord> NextImpl() override {
        PreparedIndexState state = prepare_update_state();

        auto scan_type = leaf_scan_type(scan_executor_.get());

        if (scan_type == "SeqScanExecutor" || scan_type == "IndexScanExecutor") {
            execute_buffered_update_path(state);
            return nullptr;
        }

        throw InternalError("UpdateExecutor requires a scan executor child");
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
