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
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::unique_ptr<AbstractExecutor> scan_executor_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

    struct PreparedIndexState {
        std::vector<ColMeta *> set_cols;
        std::vector<IxIndexHandle *> index_handles;
        int record_size = 0;
    };

    /**
     * @brief Prepare metadata shared by both update execution paths.
     */
    auto prepare_update_state() -> PreparedIndexState {
        PreparedIndexState state;
        state.set_cols.reserve(set_clauses_.size());
        for (auto &set_clause : set_clauses_) {
            auto &col = *tab_.get_col(set_clause.lhs.col_name);
            if (col.type != set_clause.rhs.type) {
                throw IncompatibleTypeError(coltype2str(col.type), coltype2str(set_clause.rhs.type));
            }
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
     * @brief Validate a unique index before any old key is removed for the row.
     */
    void precheck_unique_for_row(const PreparedIndexState &state, size_t index_pos, const Rid &rid,
                                 const RmRecord &old_rec, const RmRecord &new_rec) {
        auto &index = tab_.indexes[index_pos];
        if (!index.unique) {
            return;
        }

        auto new_key = build_index_key(index, new_rec);
        std::vector<Rid> result;
        if (!state.index_handles[index_pos]->get_value(new_key.get(), &result, context_->txn_)) {
            return;
        }

        auto old_key = build_index_key(index, old_rec);
        for (auto &hit : result) {
            if (hit.page_no == rid.page_no && hit.slot_no == rid.slot_no &&
                memcmp(old_key.get(), new_key.get(), index.col_tot_len) == 0) {
                continue;
            }
            throw UniqueViolationError(tab_name_, index.col_names());
        }
    }

    void precheck_unique_for_row(const PreparedIndexState &state, const Rid &rid,
                                 const RmRecord &old_rec, const RmRecord &new_rec) {
        for (size_t i = 0; i < tab_.indexes.size(); ++i) {
            precheck_unique_for_row(state, i, rid, old_rec, new_rec);
        }
    }

    /**
     * @brief Apply index maintenance and record replacement for one row.
     */
    void apply_row_update(const PreparedIndexState &state, const Rid &rid, const RmRecord &old_rec, const RmRecord &new_rec) {
        for (size_t i = 0; i < tab_.indexes.size(); ++i) {
            auto old_key = build_index_key(tab_.indexes[i], old_rec);
            state.index_handles[i]->delete_entry(old_key.get(), rid, context_->txn_);
        }

        fh_->update_record(rid, new_rec.data, context_);

        for (size_t i = 0; i < tab_.indexes.size(); ++i) {
            auto new_key = build_index_key(tab_.indexes[i], new_rec);
            state.index_handles[i]->insert_entry(new_key.get(), rid, context_->txn_);
        }
    }

    /**
     * @brief Stream updates directly from a sequential scan child.
     */
    void execute_seq_scan_path(const PreparedIndexState &state) {
        scan_executor_->beginTuple();
        for (; !scan_executor_->is_end(); scan_executor_->nextTuple()) {
            Rid rid = scan_executor_->rid();
            auto old_rec = scan_executor_->Next();
            auto new_rec = build_updated_record(state, *old_rec);

            // Sequential scans keep index state synchronized per row, so each row
            // must pass unique validation before any old index entry is removed.
            precheck_unique_for_row(state, rid, *old_rec, *new_rec);
            apply_row_update(state, rid, *old_rec, *new_rec);
        }
    }

    /**
     * @brief Preserve buffered unique-check semantics for index-scan children.
     */
    void precheck_unique_for_batch(const PreparedIndexState &state, const std::vector<Rid> &rids,
                                   const std::vector<std::unique_ptr<RmRecord>> &old_records,
                                   const std::vector<std::unique_ptr<RmRecord>> &new_records) {
        auto encode_rid = [](const Rid &rid) -> std::uint64_t {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(rid.page_no)) << 32) |
                   static_cast<std::uint32_t>(rid.slot_no);
        };
        
        std::unordered_map<std::uint64_t, size_t> row_by_rid;
        row_by_rid.reserve(rids.size());
        for (size_t row = 0; row < rids.size(); ++row) {
            row_by_rid.emplace(encode_rid(rids[row]), row);
        }

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
                // step 1: check for duplicate new key
                if (!inserted && rids[owner_it->second] != rids[row]) {
                    throw UniqueViolationError(tab_name_, index.col_names());
                }

                std::vector<Rid> result;
                // step 2: check if the new key exists in the index
                if (!state.index_handles[i]->get_value(new_key_bytes[row].data(), &result, context_->txn_)) {
                    continue;
                }

                for (auto &hit : result) {
                    // step 3.1: check if the hit is the same as the current row, allowing same RID if it's the same row
                    if (hit.page_no == rids[row].page_no && hit.slot_no == rids[row].slot_no &&
                        old_key_bytes[row] == new_key_bytes[row]) {
                        continue;
                    }
                    // step 3.2: check if the hit is a duplicate of a different row
                    auto hit_row = row_by_rid.find(encode_rid(hit));
                    if (hit_row != row_by_rid.end() &&
                        new_key_bytes[hit_row->second] != new_key_bytes[row]) {
                        // 这条命中记录虽然此刻还占着这个键，但等批量更新真正落地时，
                        // 它会改去别的键，这个位置会被腾出来
                        continue;
                    }
                    throw UniqueViolationError(tab_name_, index.col_names());
                }
            }
        }
    }

    /**
     * @brief Drain the full index-scan result before mutating any index entry.
     */
    void execute_index_scan_path(const PreparedIndexState &state) {
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

        // Index scans may revisit keys that will be changed later in the same
        // statement, so the full child result must be consumed before any index
        // deletion in order to preserve batch-level atomicity.
        precheck_unique_for_batch(state, rids, old_records, new_records);

        for (size_t row = 0; row < rids.size(); ++row) {
            apply_row_update(state, rids[row], *old_records[row], *new_records[row]);
        }
    }

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::unique_ptr<AbstractExecutor> scan_executor, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        scan_executor_ = std::move(scan_executor);
        context_ = context;
    }

    /**
     * @brief Execute hybrid update consumption based on the child scan type.
     *
     * Sequential scans persist row by row.
     * Index scans are fully consumed first to keep the scan cursor stable.
     */
    std::unique_ptr<RmRecord> Next() override {
        PreparedIndexState state = prepare_update_state();

        if (scan_executor_->getType() == "SeqScanExecutor") {
            execute_seq_scan_path(state);
            return nullptr;
        }

        if (scan_executor_->getType() == "IndexScanExecutor") {
            execute_index_scan_path(state);
            return nullptr;
        }

        throw InternalError("UpdateExecutor requires a scan executor child");
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
