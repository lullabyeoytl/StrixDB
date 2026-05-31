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
    std::vector<Condition> conds_;
    std::unique_ptr<RmRecord> current_;
    Rid rid_;

    void set_end() {
        current_.reset();
        rid_ = Rid{-1, -1};
    }

    void seek_to_next_valid() {
        while (!prev_->is_end()) {
            auto record = prev_->Next();
            if (record != nullptr && evaluate_conditions(conds_, *record, prev_->cols())) {
                current_ = std::move(record);
                rid_ = prev_->rid();
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
        set_end();
    }

    void beginTupleImpl() override {
        prev_->beginTuple();
        seek_to_next_valid();
    }

    void restartTupleImpl() override {
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
