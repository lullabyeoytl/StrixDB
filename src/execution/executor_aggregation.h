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
#include <cstring>
#include <map>
#include <sstream>

#include "execution_common.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "system/sm.h"

/*
 * @brief AggState holds the state of an aggregation operation
 * including group values, counts, sums, and min/max values.
 */
struct AggState {
    std::vector<Value> group_values;    // group by col value
    std::vector<int> counts;
    std::vector<double> sums;
    std::vector<bool> sum_inits;
    std::vector<Value> min_vals;
    std::vector<Value> max_vals;
    std::vector<bool> min_inits;
    std::vector<bool> max_inits;
};

class AggregationStateHelper {
   protected:
    std::vector<AggInfo> agg_infos_;
    std::vector<TabCol> group_by_cols_;
    std::vector<HavingCond> having_conds_;
    std::vector<ColMeta> cols_;
    size_t len_ = 0;

    ColType agg_result_type(const std::vector<ColMeta> &input_cols, const AggInfo &agg) const {
        if (agg.agg_type == AGG_COUNT) {
            return TYPE_INT;
        }
        const auto &input_col = find_col_meta(input_cols, agg.col);
        if (agg.agg_type == AGG_AVG) {
            return TYPE_FLOAT;
        }
        return input_col.type;
    }

    int agg_result_len(const std::vector<ColMeta> &input_cols, const AggInfo &agg) const {
        ColType result_type = agg_result_type(input_cols, agg);
        if (result_type == TYPE_INT) {
            return sizeof(int);
        }
        if (result_type == TYPE_FLOAT) {
            return sizeof(float);
        }
        return find_col_meta(input_cols, agg.col).len;
    }

    AggState empty_state() const {
        AggState state;
        state.counts.assign(agg_infos_.size(), 0);
        state.sums.assign(agg_infos_.size(), 0);
        state.sum_inits.assign(agg_infos_.size(), false);
        state.min_vals.resize(agg_infos_.size());
        state.max_vals.resize(agg_infos_.size());
        state.min_inits.assign(agg_infos_.size(), false);
        state.max_inits.assign(agg_infos_.size(), false);
        return state;
    }

    Value default_value_for_type(ColType type) const {
        Value result;
        if (type == TYPE_INT) {
            result.set_int(0);
        } else if (type == TYPE_FLOAT) {
            result.set_float(0.0f);
        } else if (type == TYPE_STRING) {
            result.set_str(std::string());
        } else if (type == TYPE_DATETIME) {
            result.set_datetime(0);
        } else {
            throw InternalError("Unexpected aggregate result type");
        }
        return result;
    }

    AggState new_state(const std::vector<ColMeta> &input_cols, const RmRecord &record) const {
        AggState state = empty_state();
        for (auto &group_col : group_by_cols_) {
            state.group_values.push_back(get_col_value(record, find_col_meta(input_cols, group_col)));
        }
        return state;
    }

    std::string value_key_part(const Value &value) const {
        std::ostringstream os;
        os << static_cast<int>(value.type) << ':';
        if (value.type == TYPE_INT) {
            os << value.int_val;
        } else if (value.type == TYPE_FLOAT) {
            os << value.float_val;
        } else if (value.type == TYPE_STRING) {
            os << value.str_val.size() << ':' << value.str_val;
        } else if (value.type == TYPE_DATETIME) {
            os << value.datetime_val;
        } else {
            throw InternalError("Unexpected group key value type");
        }
        return os.str();
    }

    std::string group_key(const std::vector<ColMeta> &input_cols, const RmRecord &record) const {
        std::string key;
        for (auto &group_col : group_by_cols_) {
            auto value = get_col_value(record, find_col_meta(input_cols, group_col));
            key += value_key_part(value);
            key.push_back('|');
        }
        return key;
    }

    void update_state(const std::vector<ColMeta> &input_cols, AggState &state, const RmRecord &record) {
        for (size_t i = 0; i < agg_infos_.size(); i++) {
            const auto &agg = agg_infos_[i];
            Value value;
            if (agg.is_star) {
                value.set_int(1);
            } else {
                value = get_col_value(record, find_col_meta(input_cols, agg.col));
            }

            switch (agg.agg_type) {
                case AGG_COUNT:
                    state.counts[i]++;
                    break;
                case AGG_SUM:
                case AGG_AVG:
                    state.sum_inits[i] = true;
                    state.counts[i]++;
                    state.sums[i] += value.type == TYPE_INT ? value.int_val : value.float_val;
                    break;
                case AGG_MIN:
                    if (!state.min_inits[i] || compare_values(value, state.min_vals[i], OP_LT)) {
                        state.min_vals[i] = value;
                        state.min_inits[i] = true;
                    }
                    break;
                case AGG_MAX:
                    if (!state.max_inits[i] || compare_values(state.max_vals[i], value, OP_LT)) {
                        state.max_vals[i] = value;
                        state.max_inits[i] = true;
                    }
                    break;
            }
        }
    }

    Value agg_value(const std::vector<ColMeta> &input_cols, const AggState &state,
                    const AggInfo &agg, size_t agg_idx) const {
        ColType result_type = agg_result_type(input_cols, agg);
        Value result;
        switch (agg.agg_type) {
            case AGG_COUNT:
                result.set_int(state.counts[agg_idx]);
                return result;
            case AGG_SUM:
                if (result_type == TYPE_INT) {
                    result.set_int(static_cast<int>(state.sums[agg_idx]));
                } else {
                    result.set_float(static_cast<float>(state.sums[agg_idx]));
                }
                return result;
            case AGG_AVG:
                if (state.counts[agg_idx] == 0) {
                    return default_value_for_type(result_type);
                }
                result.set_float(static_cast<float>(state.sums[agg_idx] / state.counts[agg_idx]));
                return result;
            case AGG_MIN:
                if (!state.min_inits[agg_idx]) {
                    return default_value_for_type(result_type);
                }
                return state.min_vals[agg_idx];
            case AGG_MAX:
                if (!state.max_inits[agg_idx]) {
                    return default_value_for_type(result_type);
                }
                return state.max_vals[agg_idx];
        }
        throw InternalError("Unexpected aggregate type");
    }

    bool passes_having(const std::vector<ColMeta> &input_cols, const AggState &state) const {
        for (auto &cond : having_conds_) {
            Value lhs;
            if (cond.is_agg) {
                auto it = std::find_if(agg_infos_.begin(), agg_infos_.end(),
                                        [&](const AggInfo &agg) { return agg.equals(cond.agg); });
                if (it == agg_infos_.end()) {
                    throw InternalError("AggregationExecutor guaranteed HAVING aggregate state");
                }
                lhs = agg_value(input_cols, state, *it, it - agg_infos_.begin());
            } else {
                auto it = std::find_if(group_by_cols_.begin(), group_by_cols_.end(),
                                        [&](const TabCol &col) { return col.equals(cond.col); });
                if (it == group_by_cols_.end()) {
                    throw InternalError("Analyze guaranteed HAVING group column existence");
                }
                lhs = state.group_values[it - group_by_cols_.begin()];
            }
            if (!compare_values(lhs, cond.rhs_val, cond.op)) {
                return false;
            }
        }
        return true;
    }

    void write_value(char *dest, int len, Value value) const {
        value.init_raw(len);
        memcpy(dest, value.raw->data, len);
    }

    void build_output_cols(const std::vector<ColMeta> &input_cols) {
        cols_.clear();
        size_t curr_offset = 0;
        for (auto &group_col : group_by_cols_) {
            auto col = find_col_meta(input_cols, group_col);
            col.offset = curr_offset;
            curr_offset += col.len;
            cols_.push_back(col);
        }
        for (auto &agg : agg_infos_) {
            ColMeta col;
            col.tab_name = "";
            col.name = agg_output_name(agg);
            col.type = agg_result_type(input_cols, agg);
            col.len = agg_result_len(input_cols, agg);
            col.offset = curr_offset;
            col.index = false;
            curr_offset += col.len;
            cols_.push_back(col);
        }
        len_ = curr_offset;
    }

    std::unique_ptr<RmRecord> make_output_record(const std::vector<ColMeta> &input_cols, const AggState &state) const {
        auto out = std::make_unique<RmRecord>(len_);
        for (size_t i = 0; i < group_by_cols_.size(); i++) {
            write_value(out->data + cols_[i].offset, cols_[i].len, state.group_values[i]);
        }
        for (size_t i = 0; i < agg_infos_.size(); i++) {
            auto value = agg_value(input_cols, state, agg_infos_[i], i);
            write_value(out->data + cols_[group_by_cols_.size() + i].offset,
                        cols_[group_by_cols_.size() + i].len, value);
        }
        return out;
    }
};

class AggregationExecutor : public AbstractExecutor, private AggregationStateHelper {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::map<std::string, AggState> groups_;
    std::map<std::string, AggState>::iterator iter_;
    Rid rid_;
    bool built_;

    void build_groups() {
        groups_.clear();
        prev_->beginTuple();
        for (; !prev_->is_end(); prev_->nextTuple()) {
            auto record = prev_->Next();
            auto key = group_key(prev_->cols(), *record);
            auto it = groups_.find(key);
            if (it == groups_.end()) {
                it = groups_.emplace(key, new_state(prev_->cols(), *record)).first;
            }
            update_state(prev_->cols(), it->second, *record);
        }

        // no group by and source col is none: SQL standard is to return a single row
        if (groups_.empty() && group_by_cols_.empty()) {
            groups_.emplace(std::string(), empty_state());
        }

        for (auto it = groups_.begin(); it != groups_.end();) {
            if (!passes_having(prev_->cols(), it->second)) {
                it = groups_.erase(it);
            } else {
                ++it;
            }
        }
    }

   public:
    AggregationExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<AggInfo> agg_infos,
                        std::vector<TabCol> group_by_cols, std::vector<HavingCond> having_conds) {
        prev_ = std::move(prev);
        agg_infos_ = std::move(agg_infos);
        group_by_cols_ = std::move(group_by_cols);
        having_conds_ = std::move(having_conds);
        built_ = false;
        rid_ = Rid{-1, -1};
        build_output_cols(prev_->cols());
    }

    void beginTuple() override {
        build_groups();
        iter_ = groups_.begin();
        built_ = true;
    }

    void nextTuple() override {
        if (!built_ || iter_ == groups_.end()) {
            return;
        }
        ++iter_;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return make_output_record(prev_->cols(), iter_->second);
    }

    Rid &rid() override { return rid_; }

    bool is_end() const override {
        return !built_ || iter_ == groups_.end();
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }
};
