#pragma once

#include <memory>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "system/sm.h"

class LimitExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> cols_;
    size_t len_ = 0;
    size_t limit_ = 0;
    size_t offset_ = 0;
    size_t emitted_ = 0;
    bool positioned_ = false;

    void skip_offset_rows() {
        size_t skipped = 0;
        while (skipped < offset_ && !prev_->is_end()) {
            prev_->Next();
            prev_->nextTuple();
            ++skipped;
        }
    }

    void refresh_rid() {
        if (is_end()) {
            _abstract_rid = Rid{-1, -1};
            return;
        }
        _abstract_rid = prev_->rid();
    }

   public:
    LimitExecutor(std::unique_ptr<AbstractExecutor> prev, const LimitSpec &limit_spec) {
        prev_ = std::move(prev);
        set_children({prev_.get()});
        cols_ = prev_->cols();
        if (!cols_.empty()) {
            len_ = cols_.back().offset + cols_.back().len;
        }
        limit_ = limit_spec.limit;
        offset_ = limit_spec.offset;
    }

    void beginTupleImpl() override {
        emitted_ = 0;
        positioned_ = true;
        prev_->beginTuple();
        if (limit_ == 0) {
            refresh_rid();
            return;
        }
        skip_offset_rows();
        refresh_rid();
    }

    void nextTuple() override {
        if (is_end()) {
            return;
        }
        ++emitted_;
        prev_->nextTuple();
        refresh_rid();
    }

    std::unique_ptr<RmRecord> NextImpl() override {
        if (is_end()) {
            return nullptr;
        }
        auto tuple = prev_->Next();
        if (tuple != nullptr) {
            _abstract_rid = prev_->rid();
        }
        return tuple;
    }

    Rid &rid() override { return _abstract_rid; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    bool is_end() const override {
        if (!positioned_) {
            return true;
        }
        return emitted_ >= limit_ || prev_->is_end();
    }
};
