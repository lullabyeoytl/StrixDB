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

#include "join_common.h"
#include "executor_abstract.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    JoinType join_type_ = INNER_JOIN;
    bool reverse_right_scan_ = false;
    size_t eval_len_ = 0;
    std::vector<ColMeta> eval_cols_;
    size_t output_len_ = 0;
    std::vector<ColMeta> output_cols_;

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;
    size_t left_len_ = 0;
    size_t right_len_ = 0;
    Rid left_rid_{-1, -1};
    std::unique_ptr<RmRecord> left_rec_;
    std::unique_ptr<RmRecord> right_rec_;
    std::vector<RmRecord> right_records_;
    std::vector<Rid> right_rids_;
    size_t right_pos_ = 0;

    void set_end() {
        isend = true;
        left_rid_ = Rid{-1, -1};
        left_rec_.reset();
        right_rec_.reset();
        right_pos_ = 0;
    }

    auto current_pair_matches() const -> bool {
        if (left_rec_ == nullptr || right_rec_ == nullptr) {
            return false;
        }
        return evaluate_join_pair_conditions(fed_conds_, *left_rec_, *right_rec_, left_len_, right_len_, eval_cols_);
    }

    void begin_right_scan() {
        if (!reverse_right_scan_) {
            right_->beginTuple();
            return;
        }
        right_pos_ = 0;
        if (right_records_.empty()) {
            return;
        }
        right_rec_ = std::make_unique<RmRecord>(right_records_[right_pos_]);
        _abstract_rid = right_rids_[right_pos_];
    }

    bool right_scan_end() const {
        if (!reverse_right_scan_) {
            return right_->is_end();
        }
        return right_pos_ >= right_records_.size();
    }

    void advance_right_scan() {
        if (!reverse_right_scan_) {
            right_->nextTuple();
            return;
        }
        right_pos_++;
        if (right_pos_ < right_records_.size()) {
            right_rec_ = std::make_unique<RmRecord>(right_records_[right_pos_]);
            _abstract_rid = right_rids_[right_pos_];
        }
    }

    void load_right_record() {
        if (!reverse_right_scan_) {
            right_rec_ = right_->Next();
        }
    }

    void cache_right_records() {
        if (!reverse_right_scan_) {
            return;
        }
        right_records_.clear();
        right_rids_.clear();
        for (right_->beginTuple(); !right_->is_end(); right_->nextTuple()) {
            auto record = right_->Next();
            if (record != nullptr) {
                right_records_.emplace_back(*record);
                right_rids_.push_back(right_->rid());
            }
        }
        std::reverse(right_records_.begin(), right_records_.end());
        std::reverse(right_rids_.begin(), right_rids_.end());
    }

    void seek_next_match(bool advance_left) {
        if (isend) {
            return;
        }

        if (advance_left) {
            left_->nextTuple();
            if (left_->is_end()) {
                set_end();
                return;
            }
            left_rid_ = left_->rid();
            left_rec_ = left_->Next();
            begin_right_scan();
        }

        while (!left_->is_end()) {
            while (!right_scan_end()) {
                load_right_record();
                if (current_pair_matches()) {
                    isend = false;
                    return;
                }
                advance_right_scan();
            }
            left_->nextTuple();
            if (left_->is_end()) {
                break;
            }
            left_rid_ = left_->rid();
            left_rec_ = left_->Next();
            begin_right_scan();
        }

        set_end();
    }

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds, JoinType join_type = INNER_JOIN,
                            bool reverse_right_scan = false) {
        left_ = std::move(left);
        right_ = std::move(right);
        join_type_ = join_type;
        reverse_right_scan_ = reverse_right_scan;
        left_len_ = left_->tupleLen();
        right_len_ = right_->tupleLen();
        auto layout = build_join_schema_layout(left_->cols(), left_len_, right_->cols(), right_len_, join_type_);
        eval_len_ = layout.eval_len;
        eval_cols_ = std::move(layout.eval_cols);
        output_len_ = layout.output_len;
        output_cols_ = std::move(layout.output_cols);
        isend = true;
        fed_conds_ = std::move(conds);

    }

    void beginTuple() override {
        left_->beginTuple();
        if (left_->is_end()) {
            set_end();
            return;
        }
        cache_right_records();
        left_rid_ = left_->rid();
        left_rec_ = left_->Next();
        begin_right_scan();
        isend = false;
        seek_next_match(false);
    }

    void nextTuple() override {
        if (isend) {
            return;
        }
        if (join_type_ == SEMI_JOIN) {
            seek_next_match(true);
            return;
        }
        advance_right_scan();
        seek_next_match(false);
    }

    std::unique_ptr<RmRecord> Next() override {
        if (isend || left_rec_ == nullptr || right_rec_ == nullptr) {
            return nullptr;
        }
        _abstract_rid = left_rid_;
        if (join_type_ == SEMI_JOIN) {
            return build_semi_output_record(*left_rec_, output_len_);
        }
        return build_join_eval_record(*left_rec_, *right_rec_, left_len_, right_len_);
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return isend; }

    size_t tupleLen() const override { return output_len_; }

    const std::vector<ColMeta> &cols() const override { return output_cols_; }
};
