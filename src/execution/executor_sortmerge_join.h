#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "executor_abstract.h"
#include "join_common.h"

class SortMergeJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    JoinType join_type_ = INNER_JOIN;
    std::vector<Condition> merge_conds_;
    std::vector<Condition> residual_conds_;
    std::vector<ColMeta> eval_cols_;
    std::vector<ColMeta> output_cols_;
    std::vector<ColMeta> left_key_metas_;
    std::vector<ColMeta> right_key_metas_;
    size_t left_len_ = 0;
    size_t right_len_ = 0;
    size_t eval_len_ = 0;
    size_t output_len_ = 0;
    Rid left_rid_{-1, -1};
    bool isend_ = true;
    std::unique_ptr<RmRecord> left_rec_;
    std::unique_ptr<RmRecord> right_rec_;
    std::vector<RmRecord> right_group_;
    size_t right_group_cursor_ = 0;
    bool group_ready_ = false;
    bool pending_semi_advance_ = false;

    auto compare_merge_keys(const RmRecord &left_rec, const RmRecord &right_rec) const -> int {
        return compare_join_keys(left_rec, left_key_metas_, right_rec, right_key_metas_);
    }

    auto pair_matches(const RmRecord &left_rec, const RmRecord &right_rec) const -> bool {
        return evaluate_join_pair_conditions(residual_conds_, left_rec, right_rec, left_len_, right_len_, eval_cols_);
    }

    void set_end() {
        isend_ = true;
        left_rid_ = Rid{-1, -1};
        left_rec_.reset();
        right_rec_.reset();
        right_group_.clear();
        right_group_cursor_ = 0;
        group_ready_ = false;
        pending_semi_advance_ = false;
    }

    void advance_left() {
        left_->nextTuple();
        if (left_->is_end()) {
            left_rec_.reset();
            left_rid_ = Rid{-1, -1};
            return;
        }
        left_rid_ = left_->rid();
        left_rec_ = left_->Next();
    }

    void advance_right() {
        right_->nextTuple();
        if (right_->is_end()) {
            right_rec_.reset();
            return;
        }
        right_rec_ = right_->Next();
    }

    void materialize_right_group() {
        right_group_.clear();
        right_group_cursor_ = 0;
        if (right_rec_ == nullptr) {
            group_ready_ = false;
            return;
        }
        right_group_.push_back(*right_rec_);
        while (true) {
            right_->nextTuple();
            if (right_->is_end()) {
                right_rec_.reset();
                break;
            }
            auto next_right = right_->Next();
            if (compare_merge_keys(*left_rec_, *next_right) != 0) {
                right_rec_ = std::move(next_right);
                break;
            }
            right_group_.push_back(*next_right);
        }
        group_ready_ = !right_group_.empty();
    }

    bool try_emit_from_group() {
        while (group_ready_ && left_rec_ != nullptr) {
            while (right_group_cursor_ < right_group_.size()) {
                if (pair_matches(*left_rec_, right_group_[right_group_cursor_])) {
                    isend_ = false;
                    if (join_type_ == SEMI_JOIN) {
                        pending_semi_advance_ = true;
                    }
                    return true;
                }
                ++right_group_cursor_;
            }
            if (join_type_ == SEMI_JOIN) {
                right_group_cursor_ = 0;
                advance_left();
                if (left_rec_ == nullptr) {
                    group_ready_ = false;
                    break;
                }
                if (compare_merge_keys(*left_rec_, right_group_.front()) != 0) {
                    group_ready_ = false;
                    right_group_.clear();
                    break;
                }
                continue;
            }
            right_group_cursor_ = 0;
            advance_left();
            if (left_rec_ == nullptr || compare_merge_keys(*left_rec_, right_group_.front()) != 0) {
                group_ready_ = false;
                break;
            }
        }
        return false;
    }

    void seek_next_match() {
        if (left_rec_ == nullptr) {
            set_end();
            return;
        }
        while (left_rec_ != nullptr) {
            // The current right_group_ remains valid even after the right stream has
            // advanced to its next distinct key or reached EOF.
            if (group_ready_ && try_emit_from_group()) {
                return;
            }
            if (right_rec_ == nullptr) {
                break;
            }
            int cmp = compare_merge_keys(*left_rec_, *right_rec_);
            if (cmp < 0) {
                advance_left();
            } else if (cmp > 0) {
                advance_right();
            } else {
                materialize_right_group();
                if (try_emit_from_group()) {
                    return;
                }
            }
        }
        set_end();
    }

    void advance_left_after_semi_emit() {
        pending_semi_advance_ = false;
        right_group_cursor_ = 0;
        advance_left();
        if (left_rec_ == nullptr) {
            set_end();
            return;
        }
        if (!right_group_.empty() && compare_merge_keys(*left_rec_, right_group_.front()) == 0) {
            group_ready_ = true;
        } else {
            group_ready_ = false;
            right_group_.clear();
        }
    }

   public:
    SortMergeJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                          std::vector<Condition> merge_conds, std::vector<Condition> residual_conds,
                          JoinType join_type = INNER_JOIN) {
        left_ = std::move(left);
        right_ = std::move(right);
        join_type_ = join_type;
        merge_conds_ = std::move(merge_conds);
        residual_conds_ = std::move(residual_conds);
        left_len_ = left_->tupleLen();
        right_len_ = right_->tupleLen();
        auto layout = build_join_schema_layout(left_->cols(), left_len_, right_->cols(), right_len_, join_type_);
        eval_len_ = layout.eval_len;
        eval_cols_ = std::move(layout.eval_cols);
        output_len_ = layout.output_len;
        output_cols_ = std::move(layout.output_cols);
        for (const auto &cond : merge_conds_) {
            left_key_metas_.push_back(find_col_meta(left_->cols(), cond.lhs_col));
            right_key_metas_.push_back(find_col_meta(right_->cols(), cond.rhs_col));
        }
    }

    void beginTuple() override {
        if (merge_conds_.empty()) {
            throw InternalError("SortMergeJoinExecutor requires at least one merge key");
        }
        left_->beginTuple();
        right_->beginTuple();
        if (left_->is_end() || right_->is_end()) {
            set_end();
            return;
        }
        left_rid_ = left_->rid();
        left_rec_ = left_->Next();
        right_rec_ = right_->Next();
        isend_ = false;
        group_ready_ = false;
        right_group_.clear();
        right_group_cursor_ = 0;
        pending_semi_advance_ = false;
        seek_next_match();
    }

    void nextTuple() override {
        if (isend_) {
            return;
        }
        if (join_type_ == SEMI_JOIN && pending_semi_advance_) {
            advance_left_after_semi_emit();
            seek_next_match();
            return;
        }
        if (group_ready_) {
            ++right_group_cursor_;
        }
        seek_next_match();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (isend_ || left_rec_ == nullptr) {
            return nullptr;
        }
        _abstract_rid = left_rid_;
        if (join_type_ == SEMI_JOIN) {
            return build_semi_output_record(*left_rec_, output_len_);
        }
        if (right_group_cursor_ >= right_group_.size()) {
            return nullptr;
        }
        return build_join_eval_record(*left_rec_, right_group_[right_group_cursor_], left_len_, right_len_);
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return isend_; }

    size_t tupleLen() const override { return output_len_; }

    const std::vector<ColMeta> &cols() const override { return output_cols_; }
};
