#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "executor_abstract.h"
#include "join_common.h"

/**
 * Hash join key built from the planner-provided equi-join columns.
 * Numeric values are hashed using a normalized numeric domain so that
 * join semantics stay consistent with Value::compare().
 */
struct HashJoinKey {
    std::vector<Value> values;
};

struct HashJoinKeyHasher {
    auto operator()(const HashJoinKey &key) const -> size_t {
        size_t seed = 0;
        for (const auto &value : key.values) {
            size_t value_hash = 0;
            if (is_numeric_type(value.type)) {
                double normalized = value.type == TYPE_INT ? static_cast<double>(value.int_val) : value.float_val;
                value_hash = std::hash<int>{}(1);
                value_hash ^= std::hash<double>{}(normalized) + 0x9e3779b9 + (value_hash << 6) + (value_hash >> 2);
            } else if (value.type == TYPE_STRING) {
                value_hash = std::hash<int>{}(2);
                value_hash ^= std::hash<std::string>{}(value.str_val) + 0x9e3779b9 + (value_hash << 6) +
                              (value_hash >> 2);
            } else if (value.type == TYPE_DATETIME) {
                value_hash = std::hash<int>{}(3);
                value_hash ^= std::hash<int64_t>{}(value.datetime_val) + 0x9e3779b9 + (value_hash << 6) +
                              (value_hash >> 2);
            } else {
                throw InternalError("Unexpected value type in HashJoinKeyHasher");
            }
            seed ^= value_hash + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        }
        return seed;
    }
};

struct HashJoinKeyEqual {
    auto operator()(const HashJoinKey &lhs, const HashJoinKey &rhs) const -> bool {
        if (lhs.values.size() != rhs.values.size()) {
            return false;
        }
        for (size_t i = 0; i < lhs.values.size(); ++i) {
            if (lhs.values[i] != rhs.values[i]) {
                return false;
            }
        }
        return true;
    }
};

class HashJoinExecutor : public AbstractExecutor {
   private:
    // The hash table stores the fully materialized build-side tuples so probe
    // can remain streaming and re-check residual predicates on demand.
    using HashJoinTable = std::unordered_map<HashJoinKey, std::vector<RmRecord>, HashJoinKeyHasher, HashJoinKeyEqual>;

    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    JoinType join_type_ = INNER_JOIN;
    std::vector<Condition> hash_conds_;
    std::vector<Condition> residual_conds_;
    std::vector<ColMeta> eval_cols_;
    std::vector<ColMeta> output_cols_;
    std::vector<ColMeta> left_key_metas_;
    std::vector<ColMeta> right_key_metas_;
    size_t left_len_ = 0;
    size_t right_len_ = 0;
    size_t output_len_ = 0;
    Rid left_rid_{-1, -1};
    bool isend_ = true;
    std::unique_ptr<RmRecord> left_rec_;
    HashJoinTable hash_table_;
    const std::vector<RmRecord> *current_bucket_ = nullptr;
    size_t current_bucket_cursor_ = 0;
    bool pending_semi_advance_ = false;

    auto extract_join_key(const RmRecord &record, const std::vector<ColMeta> &key_metas) const -> HashJoinKey {
        return HashJoinKey{extract_join_key_values(record, key_metas)};
    }

    auto pair_matches(const RmRecord &left_rec, const RmRecord &right_rec) const -> bool {
        return evaluate_join_pair_conditions(residual_conds_, left_rec, right_rec, left_len_, right_len_, eval_cols_);
    }

    void reset_bucket_state() {
        current_bucket_ = nullptr;
        current_bucket_cursor_ = 0;
        pending_semi_advance_ = false;
    }

    void set_end() {
        isend_ = true;
        left_rid_ = Rid{-1, -1};
        left_rec_.reset();
        reset_bucket_state();
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

    /**
     * Build the in-memory hash table from the fixed right build side.
     * The planner already guarantees hash_conds_ are equi-join predicates.
     */
    void build_hash_table() {
        hash_table_.clear();
        right_->beginTuple();
        while (!right_->is_end()) {
            auto right_rec = right_->Next();
            auto key = extract_join_key(*right_rec, right_key_metas_);
            hash_table_[std::move(key)].push_back(*right_rec);
            right_->nextTuple();
        }
    }

    void load_bucket_for_left_record() {
        reset_bucket_state();
        if (left_rec_ == nullptr) {
            return;
        }
        auto key = extract_join_key(*left_rec_, left_key_metas_);
        auto it = hash_table_.find(key);
        if (it != hash_table_.end()) {
            current_bucket_ = &it->second;
        }
    }

    bool try_emit_from_bucket() {
        if (current_bucket_ == nullptr) {
            return false;
        }
        while (current_bucket_cursor_ < current_bucket_->size()) {
            if (pair_matches(*left_rec_, (*current_bucket_)[current_bucket_cursor_])) {
                isend_ = false;
                if (join_type_ == SEMI_JOIN) {
                    pending_semi_advance_ = true;
                }
                return true;
            }
            ++current_bucket_cursor_;
        }
        reset_bucket_state();
        return false;
    }

    /**
     * Probe one left tuple at a time until a visible output tuple is found.
     * For SEMI JOIN we stop after the first matching build tuple.
     */
    void seek_next_match() {
        while (left_rec_ != nullptr) {
            if (current_bucket_ == nullptr) {
                load_bucket_for_left_record();
            }
            if (try_emit_from_bucket()) {
                return;
            }
            advance_left();
        }
        set_end();
    }

    void advance_left_after_semi_emit() {
        advance_left();
        reset_bucket_state();
        if (left_rec_ == nullptr) {
            set_end();
        }
    }

   public:
    HashJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                     std::vector<Condition> hash_conds, std::vector<Condition> residual_conds,
                     JoinType join_type = INNER_JOIN) {
        left_ = std::move(left);
        right_ = std::move(right);
        join_type_ = join_type;
        hash_conds_ = std::move(hash_conds);
        residual_conds_ = std::move(residual_conds);
        left_len_ = left_->tupleLen();
        right_len_ = right_->tupleLen();
        auto layout = build_join_schema_layout(left_->cols(), left_len_, right_->cols(), right_len_, join_type_);
        eval_cols_ = std::move(layout.eval_cols);
        output_len_ = layout.output_len;
        output_cols_ = std::move(layout.output_cols);
        for (const auto &cond : hash_conds_) {
            left_key_metas_.push_back(find_col_meta(left_->cols(), cond.lhs_col));
            right_key_metas_.push_back(find_col_meta(right_->cols(), cond.rhs_col));
        }
    }

    void beginTuple() override {
        if (join_type_ != INNER_JOIN && join_type_ != SEMI_JOIN) {
            throw InternalError("HashJoinExecutor only supports INNER_JOIN and SEMI_JOIN");
        }
        if (hash_conds_.empty()) {
            throw InternalError("HashJoinExecutor requires at least one hash key");
        }
        build_hash_table();
        left_->beginTuple();
        if (left_->is_end()) {
            set_end();
            return;
        }
        left_rid_ = left_->rid();
        left_rec_ = left_->Next();
        isend_ = false;
        reset_bucket_state();
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
        ++current_bucket_cursor_;
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
        if (current_bucket_ == nullptr || current_bucket_cursor_ >= current_bucket_->size()) {
            return nullptr;
        }
        return build_join_eval_record(*left_rec_, (*current_bucket_)[current_bucket_cursor_], left_len_, right_len_);
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return isend_; }

    size_t tupleLen() const override { return output_len_; }

    const std::vector<ColMeta> &cols() const override { return output_cols_; }
};
