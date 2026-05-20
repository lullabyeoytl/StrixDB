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
#include "index/ix.h"
#include "system/sm.h"
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }
    std::unique_ptr<RmRecord> Next() override {
        // Pre-compute: type check + raw buffer + column pointers (loop-invariant)
        std::vector<ColMeta *> set_cols;
        set_cols.reserve(set_clauses_.size());
        for (auto &set_clause : set_clauses_) {
            auto &col = *tab_.get_col(set_clause.lhs.col_name);
            set_clause.rhs = coerce_value_to_type(set_clause.rhs, col.type);
            if (set_clause.rhs.raw == nullptr){
                set_clause.rhs.init_raw(col.len);
            }
            set_cols.push_back(&col);
        }

        // Pre-compute index handles (loop-invariant across rows)
        int record_size = fh_->get_file_hdr().record_size;
        std::vector<IxIndexHandle *> index_handles;
        index_handles.reserve(tab_.indexes.size());
        for (size_t i = 0; i < tab_.indexes.size(); ++i) {
            index_handles.push_back(sm_manager_->get_ih(tab_name_, tab_.indexes[i].cols));
        }

        struct UpdateCandidate {
            Rid rid;
            std::unique_ptr<RmRecord> old_rec;
            std::unique_ptr<RmRecord> new_rec;
        };

        std::vector<UpdateCandidate> candidates;
        candidates.reserve(rids_.size());
        for (auto &rid : rids_) {
            auto old_rec = fh_->get_record(rid, context_);
            auto new_rec = std::make_unique<RmRecord>(record_size);
            memcpy(new_rec->data, old_rec->data, record_size);
            for (size_t i = 0; i < set_clauses_.size(); ++i) {
                auto &col = *set_cols[i];
                memcpy(new_rec->data + col.offset, set_clauses_[i].rhs.raw->data, col.len);
            }
            candidates.push_back(UpdateCandidate{rid, std::move(old_rec), std::move(new_rec)});
        }

        // Pre-check all unique indexes before mutating any row.
        for (size_t i = 0; i < tab_.indexes.size(); ++i) {
            auto &index = tab_.indexes[i];
            if (!index.unique) {
                continue;
            }

            std::vector<std::pair<std::string, Rid>> seen_new_keys;
            for (auto &candidate : candidates) {
                auto new_key = std::make_unique<char[]>(index.col_tot_len);
                index.build_key(new_key.get(), candidate.new_rec->data);
                std::string key_bytes(new_key.get(), index.col_tot_len);

                for (auto &seen : seen_new_keys) {
                    if (seen.first == key_bytes &&
                        (seen.second.page_no != candidate.rid.page_no ||
                         seen.second.slot_no != candidate.rid.slot_no)) {
                        throw UniqueViolationError(tab_name_, index.col_names());
                    }
                }
                seen_new_keys.push_back({std::move(key_bytes), candidate.rid});

                std::vector<Rid> result;
                if (index_handles[i]->get_value(new_key.get(), &result, context_->txn_)) {
                    for (auto &r : result) {
                        // Self-hit: same row, key unchanged → OK
                        if (r.page_no == candidate.rid.page_no &&
                            r.slot_no == candidate.rid.slot_no) {
                            auto old_k = std::make_unique<char[]>(index.col_tot_len);
                            index.build_key(old_k.get(), candidate.old_rec->data);
                            if (memcmp(old_k.get(), new_key.get(), index.col_tot_len) == 0) {
                                continue;  // self-hit, no-op
                            }
                        }
                        // Swap: entry belongs to a sibling candidate vacating this key → OK
                        bool vacated = false;
                        for (auto &other : candidates) {
                            if (other.rid.page_no == r.page_no &&
                                other.rid.slot_no == r.slot_no) {
                                auto other_new_key = std::make_unique<char[]>(index.col_tot_len);
                                index.build_key(other_new_key.get(), other.new_rec->data);
                                if (memcmp(other_new_key.get(), new_key.get(), index.col_tot_len) != 0) {
                                    vacated = true;
                                }
                                break;
                            }
                        }
                        if (vacated) continue;
                        throw UniqueViolationError(tab_name_, index.col_names());
                    }
                }
            }
        }

        for (auto &candidate : candidates) {
            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];
                auto old_key = std::make_unique<char[]>(index.col_tot_len);
                index.build_key(old_key.get(), candidate.old_rec->data);
                index_handles[i]->delete_entry(old_key.get(), candidate.rid, context_->txn_);
            }

            fh_->update_record(candidate.rid, candidate.new_rec->data, context_);

            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];
                auto new_key = std::make_unique<char[]>(index.col_tot_len);
                index.build_key(new_key.get(), candidate.new_rec->data);
                index_handles[i]->insert_entry(new_key.get(), candidate.rid, context_->txn_);
            }
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
