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

#include <optional>
#include <cstring>
#include <algorithm>
#include <climits>
#include <cfloat>
#include <cstdint>
#include <limits>

#include "common/common.h"
#include "execution_common.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class IndexScanExecutor : public AbstractExecutor {
   private:
   // bound for range scan
    struct RangeBound {
        bool has_value = false;
        Value value;
        // '>' '<' -> true, '>=','<=' -> false
        bool exclusive = false;
    };

    struct LookupLayout {
        int eq_prefix_len = 0;  // EQ prefix cols nums
        int range_col_idx = -1; // range cond pos
        RangeBound lower;
        RangeBound upper;
    };

    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> index_lookup_conds_; // 索引查找条件
    std::vector<Condition> residual_conds_;     // 索引扫描后的剩余过滤条件
    std::vector<Condition> all_conds_;
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据
    bool use_covering_index_ = false;
    std::vector<int> covered_key_offsets_;
    std::vector<ColMeta> index_eval_cols_;

    Rid rid_;
    std::unique_ptr<RecScan> scan_;
    std::unique_ptr<RmRecord> current_heap_record_;

    SmManager *sm_manager_;

    void set_end() {
        rid_ = Rid{-1, -1};
        current_heap_record_.reset();
    }

    auto current_index_scan() const -> const IxScan * {
        auto *ix_scan = dynamic_cast<IxScan *>(scan_.get());
        if (ix_scan == nullptr) {
            throw InternalError("Index scan cursor is unavailable");
        }
        return ix_scan;
    }

    void build_covered_key_offsets() {
        covered_key_offsets_.clear();
        covered_key_offsets_.reserve(cols_.size());
        for (const auto &output_col : cols_) {
            int offset = 0;
            bool found = false;
            for (const auto &index_col : index_meta_.cols) {
                if (col_meta_matches(index_col, TabCol{output_col.tab_name, output_col.name})) {
                    if (output_col.len != index_col.len) {
                        throw InternalError("Column length mismatch between table and index metadata");
                    }
                    covered_key_offsets_.push_back(offset);
                    found = true;
                    break;
                }
                offset += index_col.len;
            }
            if (!found) {
                throw InternalError("Covered output column is not present in index key");
            }
        }
    }

    auto make_record_from_index_key() const -> std::unique_ptr<RmRecord> {
        const auto *ix_scan = current_index_scan();
        const char *key = ix_scan->key();
        auto record = std::make_unique<RmRecord>(len_);
        for (size_t i = 0; i < cols_.size(); ++i) {
            const auto &col = cols_[i];
            assert(covered_key_offsets_[i] + col.len <= index_meta_.col_tot_len);
            assert(col.offset + col.len <= len_);
            memcpy(record->data + col.offset, key + covered_key_offsets_[i], col.len);
        }
        return record;
    }

    auto make_eval_record_from_index_key() const -> std::unique_ptr<RmRecord> {
        const auto *ix_scan = current_index_scan();
        const char *key = ix_scan->key();
        auto record = std::make_unique<RmRecord>(index_meta_.col_tot_len);
        memcpy(record->data, key, index_meta_.col_tot_len);
        return record;
    }

    auto project_visible_record(const RmRecord &heap_record) const -> std::unique_ptr<RmRecord> {
        if (!use_covering_index_) {
            return std::make_unique<RmRecord>(heap_record);
        }
        auto record = std::make_unique<RmRecord>(len_);
        for (const auto &col : cols_) {
            const auto &heap_col = find_col_meta(tab_.cols, TabCol{col.tab_name, col.name});
            memcpy(record->data + col.offset, heap_record.data + heap_col.offset, col.len);
        }
        return record;
    }

    auto fetch_visible_heap_record(const Rid &rid) -> std::unique_ptr<RmRecord> {
        if (!is_mvcc_active(context_) && context_ != nullptr && context_->lock_mgr_ != nullptr) {
            context_->lock_mgr_->lock_shared_on_record(context_->txn_, rid, fh_->GetFd());
        }
        try {
            return fh_->get_record(rid, context_);
        } catch (const RecordNotFoundError &) {
            return nullptr;
        }
    }

    void track_ssi_record_read(const Rid &rid, const RmRecord &record) {
        ::track_ssi_record_read(context_, tab_name_, all_conds_, rid, record);
    }

    auto seek_to_next_covered_tuple() -> bool {
        if (scan_ == nullptr || scan_->is_end()) {
            rid_ = Rid{-1, -1};
            return false;
        }

        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto *ix_scan = current_index_scan();
            if (ix_scan->current_visibility() == IndexVisibility::VISIBLE) {
                auto key_record = make_record_from_index_key();
                auto eval_record = make_eval_record_from_index_key();
                if (evaluate_conditions(residual_conds_, *eval_record, index_eval_cols_)) {
                    track_ssi_record_read(rid_, *key_record);
                    current_heap_record_.reset();
                    return true;
                }
            } else {
                auto heap_record = fetch_visible_heap_record(rid_);
                if (heap_record != nullptr && evaluate_conditions(all_conds_, *heap_record, tab_.cols)) {
                    track_ssi_record_read(rid_, *heap_record);
                    current_heap_record_ = std::move(heap_record);
                    return true;
                }
            }
            scan_->next();
        }

        rid_ = Rid{-1, -1};
        return false;
    }

    void seek_to_next_valid() {
        if (use_covering_index_) {
            if (!seek_to_next_covered_tuple()) {
                set_end();
            }
            return;
        }
        if (scan_ == nullptr || scan_->is_end()) {
            set_end();
            return;
        }

        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto heap_record = fetch_visible_heap_record(rid_);
            if (heap_record != nullptr && evaluate_conditions(all_conds_, *heap_record, tab_.cols)) {
                track_ssi_record_read(rid_, *heap_record);
                current_heap_record_ = std::move(heap_record);
                return;
            }
            scan_->next();
        }
        set_end();
    }

    static void fill_extreme_value(const ColMeta &col, char *dest, bool use_max) {
        switch (col.type) {
            case TYPE_INT: {
                int val = use_max ? INT_MAX : INT_MIN;
                memcpy(dest, &val, sizeof(int));
                break;
            }
            case TYPE_FLOAT: {
                float val = use_max ? FLT_MAX : -FLT_MAX;
                memcpy(dest, &val, sizeof(float));
                break;
            }
            case TYPE_STRING:
                memset(dest, use_max ? 0xFF : 0, col.len);
                break;
            case TYPE_DATETIME: {
                int64_t val = use_max ? std::numeric_limits<int64_t>::max() : std::numeric_limits<int64_t>::min();
                memcpy(dest, &val, sizeof(int64_t));
                break;
            }
            default:
                throw InternalError("Unexpected column type");
        }
    }

    static void write_value(const ColMeta &col, const Value &value, char *dest) {
        if (!value.raw) {
            throw InternalError("Index lookup value raw buffer is not initialized");
        }
        memcpy(dest, value.raw->data, col.len);
    }

    void prepare_lookup_values() {
        for (auto &cond : index_lookup_conds_) {
            if (!cond.is_rhs_val || cond.rhs_val.raw) {
                continue;
            }
            auto col_it = std::find_if(index_meta_.cols.begin(), index_meta_.cols.end(),
                                       [&](const ColMeta &col) { return col.name == cond.lhs_col.col_name; });
            if (col_it != index_meta_.cols.end() && cond.rhs_val.type == col_it->type) {
                cond.rhs_val.init_raw(col_it->len);
            }
        }
    }

    auto find_eq_condition(const std::string &col_name) const -> const Condition * {
        auto it = std::find_if(index_lookup_conds_.begin(), index_lookup_conds_.end(), [&](const Condition &cond) {
            return cond.is_rhs_val && cond.op == OP_EQ && cond.lhs_col.col_name == col_name;
        });
        return it == index_lookup_conds_.end() ? nullptr : &(*it);
    }

    auto analyze_lookup_layout() const -> LookupLayout {
        LookupLayout layout;
        // Scan index columns in order: consume the EQ prefix first, then at most one
        // range column, and stop. Columns after the first non-EQ position stay in
        // residual filters because the B+ tree cannot narrow them further.
        for (size_t i = 0; i < index_meta_.cols.size(); ++i) {
            const auto &index_col = index_meta_.cols[i];
            if (const auto *eq_cond = find_eq_condition(index_col.name); eq_cond != nullptr) {
                ++layout.eq_prefix_len;
                continue;
            }

            bool saw_range = false;
            for (const auto &cond : index_lookup_conds_) {
                if (!cond.is_rhs_val || cond.lhs_col.col_name != index_col.name) {
                    continue;
                }
                if (cond.op == OP_GT || cond.op == OP_GE) {
                    if (!layout.lower.has_value) {
                        layout.lower = {.has_value = true, .value = cond.rhs_val, .exclusive = cond.op == OP_GT};
                    } else {
                        int cmp = cond.rhs_val.compare(layout.lower.value);
                        if (cmp > 0 || (cmp == 0 && cond.op == OP_GT && !layout.lower.exclusive)) {
                            layout.lower = {.has_value = true, .value = cond.rhs_val, .exclusive = cond.op == OP_GT};
                        }
                    }
                    saw_range = true;
                } else if (cond.op == OP_LT || cond.op == OP_LE) {
                    if (!layout.upper.has_value) {
                        layout.upper = {.has_value = true, .value = cond.rhs_val, .exclusive = cond.op == OP_LT};
                    } else {
                        int cmp = cond.rhs_val.compare(layout.upper.value);
                        if (cmp < 0 || (cmp == 0 && cond.op == OP_LT && !layout.upper.exclusive)) {
                            layout.upper = {.has_value = true, .value = cond.rhs_val, .exclusive = cond.op == OP_LT};
                        }
                    }
                    saw_range = true;
                }
            }
            if (saw_range) {
                layout.range_col_idx = static_cast<int>(i);
            }
            break;
        }
        return layout;
    }

    static auto empty_range(const LookupLayout &layout) -> bool {
        if (!layout.lower.has_value || !layout.upper.has_value) {
            return false;
        }
        int cmp = layout.lower.value.compare(layout.upper.value);
        if (cmp > 0) {
            return true;
        }
        return cmp == 0 && (layout.lower.exclusive || layout.upper.exclusive);
    }

    static void normalize_conds(std::vector<Condition> &conds, const std::string &tab_name) {
        for (auto &cond : conds) {
            if (cond.lhs_col.tab_name != tab_name) {
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name);
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = kSwapOp.at(cond.op);
            }
        }
    }

    auto build_key(const LookupLayout &layout, bool upper_side, bool use_bound_value) -> std::vector<char> {
        std::vector<char> key(index_meta_.col_tot_len, 0);
        int offset = 0;
        for (size_t i = 0; i < index_meta_.cols.size(); ++i) {
            const auto &index_col = index_meta_.cols[i];
            if (const auto *eq_cond = find_eq_condition(index_col.name); eq_cond != nullptr) {
                write_value(index_col, eq_cond->rhs_val, key.data() + offset);
            } else if (layout.range_col_idx == static_cast<int>(i)) {
                if (use_bound_value) {
                    const auto &bound = upper_side ? layout.upper : layout.lower;
                    write_value(index_col, bound.value, key.data() + offset);
                } else if (upper_side) {
                    fill_extreme_value(index_col, key.data() + offset, true);
                } else {
                    fill_extreme_value(index_col, key.data() + offset, false);
                }
            } else if (layout.range_col_idx == -1 && static_cast<int>(i) >= layout.eq_prefix_len) {
                if (upper_side) {
                    fill_extreme_value(index_col, key.data() + offset, true);
                } else {
                    fill_extreme_value(index_col, key.data() + offset, false);
                }
            } else if (layout.range_col_idx != -1 && static_cast<int>(i) > layout.range_col_idx) {
                bool use_upper_fill = upper_side;
                if (use_bound_value) {
                    if (upper_side) {
                        use_upper_fill = !layout.upper.exclusive;
                    } else {
                        use_upper_fill = layout.lower.exclusive;
                    }
                }
                if (use_upper_fill) {
                    fill_extreme_value(index_col, key.data() + offset, true);
                } else {
                    fill_extreme_value(index_col, key.data() + offset, false);
                }
            } else {
                throw InternalError("Unexpected index key layout in IndexScanExecutor::build_key");
            }
            offset += index_col.len;
        }
        return key;
    }

    auto build_scan_upper_bound(const LookupLayout &layout) -> ScanUpperBound {
        ScanUpperBound upper_bound;
        if (layout.range_col_idx == -1) {
            upper_bound.has_bound = true;
            upper_bound.key = build_key(layout, true, false);
            upper_bound.inclusive = true;
            return upper_bound;
        }

        if (layout.upper.has_value) {
            upper_bound.has_bound = true;
            upper_bound.key = build_key(layout, true, true);
            upper_bound.inclusive = !layout.upper.exclusive;
            return upper_bound;
        }

        if (layout.eq_prefix_len > 0) {
            upper_bound.has_bound = true;
            upper_bound.key = build_key(layout, true, false);
            upper_bound.inclusive = true;
        }
        return upper_bound;
    }

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> index_lookup_conds,
                      std::vector<Condition> residual_conds, std::vector<std::string> index_col_names,
                      std::optional<IndexMeta> index_meta, bool use_covering_index,
                      std::vector<ColMeta> covered_cols, Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        index_lookup_conds_ = std::move(index_lookup_conds);
        residual_conds_ = std::move(residual_conds);
        index_col_names_ = std::move(index_col_names);
        if (index_meta.has_value()) {
            index_meta_ = *index_meta;
        } else {
            index_meta_ = *(tab_.get_index_meta(index_col_names_));
        }
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        use_covering_index_ = use_covering_index;
        if (use_covering_index_) {
            cols_ = std::move(covered_cols);
            len_ = cols_.empty() ? 0 : cols_.back().offset + cols_.back().len;
            build_covered_key_offsets();
            int eval_offset = 0;
            index_eval_cols_.clear();
            for (auto col : index_meta_.cols) {
                col.offset = eval_offset;
                eval_offset += col.len;
                index_eval_cols_.push_back(std::move(col));
            }
        }

        normalize_conds(index_lookup_conds_, tab_name_);
        normalize_conds(residual_conds_, tab_name_);
        all_conds_ = index_lookup_conds_;
        all_conds_.insert(all_conds_.end(), residual_conds_.begin(), residual_conds_.end());
        prepare_lookup_values();
        if (context_ != nullptr && context_->lock_mgr_ != nullptr) {
            context_->lock_mgr_->lock_IS_on_table(context_->txn_, fh_->GetFd());
        }
        set_end();
    }

    void beginTupleImpl() override {
        restartTupleImpl();
    }

    void restartTupleImpl() override {
        track_ssi_predicate_read(context_, tab_name_, all_conds_);

        auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_col_names_);
        auto *ih = sm_manager_->ihs_.at(ix_name).get();

        LookupLayout layout = analyze_lookup_layout();
        if (empty_range(layout)) {
            scan_.reset();
            set_end();
            return;
        }

        Iid lower = ih->leaf_begin();
        ScanUpperBound upper_bound = build_scan_upper_bound(layout);

        if (layout.range_col_idx == -1) {
            auto lower_key = build_key(layout, false, false);
            lower = ih->lower_bound(lower_key.data());
        } else {
            if (layout.lower.has_value) {
                auto lower_key = build_key(layout, false, true);
                lower = layout.lower.exclusive ? ih->upper_bound(lower_key.data()) : ih->lower_bound(lower_key.data());
            } else if (layout.eq_prefix_len > 0) {
                auto lower_key = build_key(layout, false, false);
                lower = ih->lower_bound(lower_key.data());
            }

        }

        scan_ = std::make_unique<IxScan>(ih, lower, std::move(upper_bound), sm_manager_->get_bpm(),
                                         context_ == nullptr ? nullptr : context_->txn_);
        seek_to_next_valid();
    }

    void nextTuple() override {
        if (!scan_ || scan_->is_end()) {
            set_end();
            return;
        }
        scan_->next();
        seek_to_next_valid();
    }

    std::unique_ptr<RmRecord> NextImpl() override {
        if (is_end()) {
            return nullptr;
        }
        if (use_covering_index_) {
            auto *ix_scan = current_index_scan();
            if (ix_scan->current_visibility() == IndexVisibility::VISIBLE) {
                return make_record_from_index_key();
            }
            auto heap_record = current_heap_record_ != nullptr ? std::move(current_heap_record_)
                                                               : fetch_visible_heap_record(rid_);
            if (heap_record == nullptr) return nullptr;
            return project_visible_record(*heap_record);
        }
        if (current_heap_record_ != nullptr) {
            return std::move(current_heap_record_);
        }
        return fetch_visible_heap_record(rid_);
    }

    Rid &rid() override { return rid_; }

    bool is_end() const override {
        return rid_.page_no == -1 && rid_.slot_no == -1;
    }

    // Expose the scan kind so upper DML executors can choose their row consumption strategy.
    std::string getType() override { return "IndexScanExecutor"; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }
};
