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

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "execution_defs.h"
#include "execution_common.h"
#include "common/common.h"
#include "index/ix.h"
#include "system/sm.h"

class AbstractExecutor {
   private:
    size_t rows_ = 0;
    std::vector<AbstractExecutor *> children_;
    std::string explain_name_;
    std::string explain_attrs_;

   protected:
    void set_children(std::vector<AbstractExecutor *> children) {
        children_ = std::move(children);
    }

    virtual void beginTupleImpl(){};

    virtual void restartTupleImpl() { beginTupleImpl(); };

    virtual std::unique_ptr<RmRecord> NextImpl() = 0;

   public:
    Rid _abstract_rid;

    Context *context_;

    virtual ~AbstractExecutor() = default;

    virtual size_t tupleLen() const { return 0; };

    virtual const std::vector<ColMeta> &cols() const {
        throw InternalError("Executor does not expose output columns");
    };

    virtual std::string getType() { return "AbstractExecutor"; };

    void set_explain_info(std::string name, std::string attrs = std::string()) {
        explain_name_ = std::move(name);
        explain_attrs_ = std::move(attrs);
    }

    const std::string &explain_name() const { return explain_name_; }

    const std::string &explain_attrs() const { return explain_attrs_; }

    void beginTuple() {
        reset_rows();
        beginTupleImpl();
    };

    void restartTuple() { restartTupleImpl(); };

    virtual void nextTuple(){};

    virtual bool is_end() const { return true; };

    virtual size_t rows() const { return rows_; }

    void reset_rows() {
        rows_ = 0;
        for (auto *child : children_) {
            if (child != nullptr) {
                child->reset_rows();
            }
        }
    }

    std::vector<const AbstractExecutor *> children() const {
        std::vector<const AbstractExecutor *> result;
        result.reserve(children_.size());
        for (const auto *child : children_) {
            result.push_back(child);
        }
        return result;
    }

    virtual void bind_outer_tuple(const RmRecord *record, const std::vector<ColMeta> *cols) {
        (void)record;
        (void)cols;
    }

    virtual Rid &rid() = 0;

    std::unique_ptr<RmRecord> Next() {
        auto record = NextImpl();
        if (record != nullptr) {
            ++rows_;
        }
        return record;
    }

    virtual ColMeta get_col_offset(const TabCol &target) { return ColMeta();};

    std::vector<ColMeta>::const_iterator get_col(const std::vector<ColMeta> &rec_cols, const TabCol &target) {
        auto pos = std::find_if(rec_cols.begin(), rec_cols.end(),
                                [&](const ColMeta &col) { return col_meta_matches(col, target); });
        if (pos == rec_cols.end()) {
            throw ColumnNotFoundError(target.tab_name + '.' + target.col_name);
        }
        return pos;
    }
};
