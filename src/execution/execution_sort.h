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
#include <utility>
#include <vector>

#include "execution_defs.h"
#include "execution_common.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> cols_;
    std::vector<ColMeta> sort_key_metas_;
    std::vector<bool> is_descs_;
    size_t len_ = 0;
    size_t cursor_ = 0;
    std::vector<RmRecord> tuples_;
    std::vector<Rid> tuple_rids_;
    std::vector<size_t> order_;

    auto compare_tuple(const RmRecord &lhs, const RmRecord &rhs) const -> bool {
        for (size_t i = 0; i < sort_key_metas_.size(); ++i) {
            auto lhs_val = get_col_value(lhs, sort_key_metas_[i]);
            auto rhs_val = get_col_value(rhs, sort_key_metas_[i]);
            int cmp = lhs_val.compare(rhs_val);
            if (cmp == 0) {
                continue;
            }
            return is_descs_[i] ? (cmp > 0) : (cmp < 0);
        }
        return false;
    }

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<TabCol> sort_cols, std::vector<bool> is_descs) {
        prev_ = std::move(prev);
        cols_ = prev_->cols();
        if (sort_cols.empty()) {
            throw InternalError("SortExecutor requires at least one sort key");
        }
        if (sort_cols.size() != is_descs.size()) {
            throw InternalError("SortExecutor key count does not match direction count");
        }
        for (const auto &sort_col : sort_cols) {
            sort_key_metas_.push_back(find_col_meta(cols_, sort_col));
        }
        is_descs_ = std::move(is_descs);
        if (!cols_.empty()) {
            len_ = cols_.back().offset + cols_.back().len;
        }
    }

    SortExecutor(std::unique_ptr<AbstractExecutor> prev, TabCol sort_col, bool is_desc)
        : SortExecutor(std::move(prev), std::vector<TabCol>{std::move(sort_col)}, std::vector<bool>{is_desc}) {
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
        ++cursor_;
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
