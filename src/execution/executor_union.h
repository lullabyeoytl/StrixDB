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

#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "execution_common.h"
#include "executor_abstract.h"

class UnionExecutor : public AbstractExecutor {
   private:
    std::vector<std::unique_ptr<AbstractExecutor>> children_;
    std::vector<ColMeta> cols_;
    size_t len_ = 0;
    size_t cursor_ = 0;
    std::vector<RmRecord> tuples_;
    std::vector<Rid> tuple_rids_;

    auto convert_record(const RmRecord &src, const std::vector<ColMeta> &src_cols) const -> RmRecord {
        RmRecord out(static_cast<int>(len_));
        for (size_t i = 0; i < cols_.size(); ++i) {
            auto value = get_col_value(src, src_cols[i]);
            value = coerce_value_to_type(value, cols_[i].type);
            value.init_raw(cols_[i].len);
            memcpy(out.data + cols_[i].offset, value.raw->data, cols_[i].len);
        }
        return out;
    }

    auto dedup_key(const RmRecord &record) const -> std::string {
        return std::string(record.data, record.size);
    }

    void refresh_rid() {
        if (is_end()) {
            _abstract_rid = Rid{-1, -1};
        } else {
            _abstract_rid = tuple_rids_[cursor_];
        }
    }

   public:
    UnionExecutor(std::vector<std::unique_ptr<AbstractExecutor>> children, std::vector<ColMeta> output_cols) :
            children_(std::move(children)), cols_(std::move(output_cols)) {
        len_ = cols_.empty() ? 0 : cols_.back().offset + cols_.back().len;
        std::vector<AbstractExecutor *> child_ptrs;
        child_ptrs.reserve(children_.size());
        for (auto &child : children_) {
            child_ptrs.push_back(child.get());
        }
        set_children(std::move(child_ptrs));
        refresh_rid();
    }

    void beginTupleImpl() override {
        tuples_.clear();
        tuple_rids_.clear();
        std::set<std::string> seen;

        for (auto &child : children_) {
            child->beginTuple();
            const auto &child_cols = child->cols();
            if (child_cols.size() != cols_.size()) {
                throw InternalError("UNION executor child column count mismatch");
            }
            while (!child->is_end()) {
                auto src = child->Next();
                if (src != nullptr) {
                    auto out = convert_record(*src, child_cols);
                    auto key = dedup_key(out);
                    if (seen.insert(key).second) {
                        tuples_.push_back(std::move(out));
                        tuple_rids_.push_back(child->rid());
                    }
                }
                child->nextTuple();
            }
        }
        cursor_ = 0;
        refresh_rid();
    }

    void nextTuple() override {
        if (!is_end()) {
            ++cursor_;
        }
        refresh_rid();
    }

    std::unique_ptr<RmRecord> NextImpl() override {
        if (is_end()) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(tuples_[cursor_]);
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return cursor_ >= tuples_.size(); }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }
};
