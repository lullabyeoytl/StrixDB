#pragma once

#include <memory>
#include <vector>

#include "execution/executor_aggregation.h"

class SortAggregationExecutor : public AbstractExecutor, private AggregationStateHelper {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<SortKeySpec> sort_keys_;
    std::vector<ColMeta> input_cols_;
    std::vector<ColMeta> group_cols_;
    std::vector<AggState> output_groups_;
    size_t cursor_ = 0;
    Rid rid_;
    bool built_ = false;

    bool same_group(const AggState &state, const RmRecord &record) const {
        for (size_t i = 0; i < group_by_cols_.size(); ++i) {
            auto value = get_col_value(record, group_cols_[i]);
            if (!state.group_values[i].equals(value)) {
                return false;
            }
        }
        return true;
    }

    void finish_group(std::vector<AggState> &groups, AggState &state) {
        if (passes_having(input_cols_, state)) {
            groups.push_back(state);
        }
    }

    void validate_sort_keys() const {
        if (group_by_cols_.empty()) {
            throw InternalError("SortAggregationExecutor requires grouping keys");
        }
        if (sort_keys_.size() < group_by_cols_.size()) {
            throw InternalError("SortAggregationExecutor requires grouping sort keys");
        }
        for (size_t i = 0; i < group_by_cols_.size(); ++i) {
            if (!sort_keys_[i].col.equals(group_by_cols_[i])) {
                throw InternalError("SortAggregationExecutor requires grouping sort keys");
            }
        }
    }

    void build_groups() {
        std::vector<AggState> next_output_groups;
        prev_->beginTuple();

        bool has_group = false;
        AggState current = empty_state();

        for (; !prev_->is_end(); prev_->nextTuple()) {
            auto record = prev_->Next();
            if (record == nullptr) {
                continue;
            }
            if (!has_group) {
                current = new_state(input_cols_, *record);
                update_state(input_cols_, current, *record);
                has_group = true;
                continue;
            }
            if (!same_group(current, *record)) {
                finish_group(next_output_groups, current);
                current = new_state(input_cols_, *record);
            }
            update_state(input_cols_, current, *record);
        }

        if (has_group) {
            finish_group(next_output_groups, current);
        }

        output_groups_ = std::move(next_output_groups);
    }

   public:
    SortAggregationExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<AggInfo> agg_infos,
                            std::vector<TabCol> group_by_cols, std::vector<HavingCond> having_conds,
                            std::vector<SortKeySpec> sort_keys) {
        prev_ = std::move(prev);
        set_children({prev_.get()});
        agg_infos_ = std::move(agg_infos);
        group_by_cols_ = std::move(group_by_cols);
        having_conds_ = std::move(having_conds);
        sort_keys_ = std::move(sort_keys);
        input_cols_ = prev_->cols();
        validate_sort_keys();
        group_cols_.reserve(group_by_cols_.size());
        for (const auto &group_col : group_by_cols_) {
            group_cols_.push_back(find_col_meta(input_cols_, group_col));
        }
        build_output_cols(input_cols_);
        rid_ = Rid{-1, -1};
    }

    void beginTupleImpl() override {
        built_ = false;
        cursor_ = 0;
        build_groups();
        built_ = true;
    }

    void nextTuple() override {
        if (!is_end()) {
            ++cursor_;
        }
    }

    std::unique_ptr<RmRecord> NextImpl() override {
        if (is_end()) {
            return nullptr;
        }
        return make_output_record(input_cols_, output_groups_[cursor_]);
    }

    Rid &rid() override { return rid_; }

    bool is_end() const override {
        return !built_ || cursor_ >= output_groups_.size();
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }
};
