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
#include <cassert>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "defs.h"
#include "record/rm_defs.h"


struct TabCol {
    std::string tab_name;
    std::string col_name;

    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }
};

inline bool same_col(const TabCol &lhs, const TabCol &rhs) {
    return lhs.tab_name == rhs.tab_name && lhs.col_name == rhs.col_name;
}

inline bool contains_col(const std::vector<TabCol> &cols, const TabCol &target) {
    return std::any_of(cols.begin(), cols.end(), [&](const TabCol &col) { return same_col(col, target); });
}

enum AggType {
    AGG_COUNT, AGG_SUM, AGG_AVG, AGG_MIN, AGG_MAX
};

struct Value {
    ColType type;  // type of value
    union {
        int int_val;      // int value
        float float_val;  // float value
    };
    std::string str_val;  // string value

    std::shared_ptr<RmRecord> raw;  // raw record buffer

    void set_int(int int_val_) {
        type = TYPE_INT;
        int_val = int_val_;
    }

    void set_float(float float_val_) {
        type = TYPE_FLOAT;
        float_val = float_val_;
    }

    void set_str(std::string str_val_) {
        type = TYPE_STRING;
        str_val = std::move(str_val_);
    }
    
    void init_raw(int len) {
        assert(raw == nullptr);
        raw = std::make_shared<RmRecord>(len);
        if (type == TYPE_INT) {
            assert(len == sizeof(int));
            *(int *)(raw->data) = int_val;
        } else if (type == TYPE_FLOAT) {
            assert(len == sizeof(float));
            *(float *)(raw->data) = float_val;
        } else if (type == TYPE_STRING) {
            if (len < (int)str_val.size()) {
                throw StringOverflowError();
            }
            memset(raw->data, 0, len);
            memcpy(raw->data, str_val.c_str(), str_val.size());
        }
    }
};

enum CompOp { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };

struct Condition {
    TabCol lhs_col;   // left-hand side column
    CompOp op;        // comparison operator
    bool is_rhs_val;  // true if right-hand side is a value (not a column)
    TabCol rhs_col;   // right-hand side column
    Value rhs_val;    // right-hand side value
};

struct SetClause {
    TabCol lhs;
    Value rhs;
};

struct AggInfo {
    AggType agg_type;
    bool is_star;
    TabCol col;
};

struct HavingCond {
    bool is_agg;
    AggInfo agg;
    TabCol col;
    CompOp op;
    Value rhs_val;
};

inline std::string agg_output_name(const AggInfo &agg) {
    static const std::map<AggType, std::string> names = {
        {AGG_COUNT, "count"},
        {AGG_SUM, "sum"},
        {AGG_AVG, "avg"},
        {AGG_MIN, "min"},
        {AGG_MAX, "max"},
    };
    if (agg.is_star) {
        return names.at(agg.agg_type) + "(*)";
    }
    return names.at(agg.agg_type) + "(" + agg.col.col_name + ")";
}
