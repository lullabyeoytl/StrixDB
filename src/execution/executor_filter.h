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

#include "execution_common.h"
#include "executor_abstract.h"

class FilterExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<Condition> conds_;  // 条件列表
    std::unique_ptr<RmRecord> current_;
    Rid rid_;
    std::string ssi_table_name_;    // Single-table target for safe SSI filter read tracking.

    void set_end() {
        current_.reset();
        rid_ = Rid{-1, -1};
    }

    static auto same_table_name(const std::string &current, const std::string &next) -> std::string {
        if (next.empty()) {
            return current;
        }
        if (current.empty() || current == next) {
            return next;
        }
        return std::string();
    }

    static auto infer_ssi_table_name(const std::vector<Condition> &conds,
                                     const std::vector<ColMeta> &cols) -> std::string {
        std::string table_name;
        for (const auto &col : cols) {
            table_name = same_table_name(table_name, col.tab_name);
            if (table_name.empty() && !col.tab_name.empty()) {
                return std::string();
            }
        }
        for (const auto &cond : conds) {
            table_name = same_table_name(table_name, cond.lhs_col.tab_name);
            if (table_name.empty() && !cond.lhs_col.tab_name.empty()) {
                return std::string();
            }
            if (!cond.is_rhs_val) {
                table_name = same_table_name(table_name, cond.rhs_col.tab_name);
                if (table_name.empty() && !cond.rhs_col.tab_name.empty()) {
                    return std::string();
                }
            }
        }
        return table_name;
    }

    void track_filter_predicate_read() {
        if (!ssi_table_name_.empty()) {
            track_ssi_predicate_read(context_, ssi_table_name_, conds_);
        }
    }

    void track_filter_record_read(const Rid &rid, const RmRecord &record) {
        if (!ssi_table_name_.empty()) {
            track_ssi_record_read(context_, ssi_table_name_, conds_, rid, record);
        }
    }

    void seek_to_next_valid() {
        while (!prev_->is_end()) {
            auto record = prev_->Next();
            if (record != nullptr && evaluate_conditions(conds_, *record, prev_->cols())) {
                rid_ = prev_->rid();
                track_filter_record_read(rid_, *record);
                current_ = std::move(record);
                return;
            }
            prev_->nextTuple();
        }
        set_end();
    }

   public:
    FilterExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<Condition> conds)
        : prev_(std::move(prev)), conds_(std::move(conds)) {
        set_children({prev_.get()});
        context_ = prev_ == nullptr ? nullptr : prev_->context_;
        if (prev_ != nullptr) {
            ssi_table_name_ = infer_ssi_table_name(conds_, prev_->cols());
            if (!ssi_table_name_.empty()) {
                prev_->set_ssi_read_tracking_enabled(false);
            }
        }
        set_end();
    }

    void beginTupleImpl() override {
        track_filter_predicate_read();
        prev_->beginTuple();
        seek_to_next_valid();
    }

    void restartTupleImpl() override {
        track_filter_predicate_read();
        prev_->restartTuple();
        seek_to_next_valid();
    }

    void nextTuple() override {
        if (is_end()) {
            return;
        }
        prev_->nextTuple();
        seek_to_next_valid();
    }

    bool is_end() const override { return current_ == nullptr; }

    std::unique_ptr<RmRecord> NextImpl() override {
        if (current_ == nullptr) {
            return nullptr;
        }
        auto out = std::make_unique<RmRecord>(current_->size);
        memcpy(out->data, current_->data, current_->size);
        return out;
    }

    Rid &rid() override { return rid_; }

    size_t tupleLen() const override { return prev_->tupleLen(); }

    const std::vector<ColMeta> &cols() const override { return prev_->cols(); }

    std::string getType() override { return "FilterExecutor"; }

    auto child_executor() const -> AbstractExecutor * { return prev_.get(); }

};
