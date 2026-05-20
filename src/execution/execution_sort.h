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
#include <algorithm>
#include <numeric>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    ColMeta sort_col_;
    std::vector<ColMeta> cols_;
    size_t len_ = 0;
    size_t cursor_ = 0;
    bool is_desc_;
    std::vector<RmRecord> tuples_;
    std::vector<Rid> tuple_rids_;
    std::vector<size_t> order_;

    auto compare_tuple(const RmRecord &lhs, const RmRecord &rhs) const -> bool {
        auto lhs_val = get_col_value(lhs, sort_col_);
        auto rhs_val = get_col_value(rhs, sort_col_);
        int cmp = lhs_val.compare(rhs_val);
        if (cmp == 0) {
            return false;
        }
        return is_desc_ ? cmp > 0 : cmp < 0;
    }

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, TabCol sel_cols, bool is_desc) {
        prev_ = std::move(prev);
        cols_ = prev_->cols();
        sort_col_ = find_col_meta(cols_, sel_cols);
        is_desc_ = is_desc;
        if (!cols_.empty()) {
            len_ = cols_.back().offset + cols_.back().len;
        }
    }

    void beginTuple() override { 
        tuples_.clear();
        tuple_rids_.clear();
        order_.clear();

        prev_->beginTuple();
        while (!prev_->is_end()) {
            auto tuple = prev_->Next();
            if (tuple != nullptr) {
                tuples_.push_back(*tuple);
                tuple_rids_.push_back(prev_->rid());
            }
            prev_->nextTuple();
        }
        order_.resize(tuples_.size());
        std::iota(order_.begin(), order_.end(), 0);
        std::stable_sort(order_.begin(), order_.end(), [&](size_t lhs_idx, size_t rhs_idx) {
            return compare_tuple(tuples_[lhs_idx], tuples_[rhs_idx]);
        });
        cursor_ = 0;
        if (is_end()) {
            _abstract_rid = Rid{-1, -1};
        } else {
            _abstract_rid = tuple_rids_[order_[cursor_]];
        }
    }

    void nextTuple() override {
        if (is_end()) {
            return;
        }
        cursor_++;
        if (is_end()) {
            _abstract_rid = Rid{-1, -1};
        } else {
            _abstract_rid = tuple_rids_[order_[cursor_]];
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(tuples_[order_[cursor_]]);
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return cursor_ >= order_.size(); }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }
};
