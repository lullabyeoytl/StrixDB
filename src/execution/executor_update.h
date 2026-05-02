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
            if (col.type != set_clause.rhs.type) {
                throw IncompatibleTypeError(coltype2str(col.type), coltype2str(set_clause.rhs.type));
            }
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

        for (auto &rid : rids_) {
            auto rec = fh_->get_record(rid, context_);

            RmRecord new_rec(record_size);
            memcpy(new_rec.data, rec->data, record_size);
            for (size_t i = 0; i < set_clauses_.size(); ++i) {
                auto &col = *set_cols[i];
                memcpy(new_rec.data + col.offset, set_clauses_[i].rhs.raw->data, col.len);
            }

            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];
                auto old_key = std::make_unique<char[]>(index.col_tot_len);
                index.build_key(old_key.get(), rec->data);
                index_handles[i]->delete_entry(old_key.get(), rid, context_->txn_);
            }

            fh_->update_record(rid, new_rec.data, context_);

            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];
                auto new_key = std::make_unique<char[]>(index.col_tot_len);
                index.build_key(new_key.get(), new_rec.data);
                index_handles[i]->insert_entry(new_key.get(), rid, context_->txn_);
            }
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};