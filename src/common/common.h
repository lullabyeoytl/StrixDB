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
#include "errors.h"
#include "system/sm_meta.h"
#include "record/rm_defs.h"


struct TabCol {
    std::string tab_name;
    std::string col_name;

    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }
    
    inline bool equals(const TabCol &rhs) const {
        return tab_name == rhs.tab_name && col_name == rhs.col_name;
    }
};

struct SortKeySpec {
    TabCol col;
    bool is_desc = false;

    inline auto equals(const SortKeySpec &rhs) const -> bool {
        return col.equals(rhs.col) && is_desc == rhs.is_desc;
    }
};

struct LimitSpec {
    size_t limit = 0;
    size_t offset = 0;
};

inline auto make_sort_key_specs(const std::vector<TabCol> &cols, bool is_desc = false) -> std::vector<SortKeySpec> {
    std::vector<SortKeySpec> keys;
    keys.reserve(cols.size());
    for (const auto &col : cols) {
        keys.push_back(SortKeySpec{col, is_desc});
    }
    return keys;
}

inline bool contains_col(const std::vector<TabCol> &cols, const TabCol &target) {
    return std::any_of(cols.begin(), cols.end(), [&](const TabCol &col) { return col.equals(target); });
}

inline bool col_meta_matches(const ColMeta &col, const TabCol &target) {
    return col.tab_name == target.tab_name && col.name == target.col_name;
}

inline auto find_col_meta(const std::vector<ColMeta> &cols, const TabCol &target) -> const ColMeta & {
    auto it = std::find_if(cols.begin(), cols.end(),
                           [&](const ColMeta &col) { return col_meta_matches(col, target); });
    if (it == cols.end()) {
        throw ColumnNotFoundError("Column not found: " + target.tab_name + "." + target.col_name);
    }
    return *it;
}

enum AggType {
    AGG_COUNT, AGG_SUM, AGG_AVG, AGG_MIN, AGG_MAX
};

enum CompOp { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };

inline auto is_numeric_type(ColType type) -> bool {
    return type == TYPE_INT || type == TYPE_FLOAT;
}

inline auto are_comparable_types(ColType lhs, ColType rhs) -> bool {
    return lhs == rhs || (is_numeric_type(lhs) && is_numeric_type(rhs));
}

inline auto is_range_comp_op(CompOp op) -> bool {
    return op == OP_LT || op == OP_LE || op == OP_GT || op == OP_GE;
}

const std::map<CompOp, CompOp> kSwapOp = {
    {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
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

    inline auto is_numeric() const -> bool {
        return is_numeric_type(type);
    }

    inline auto compare(const Value &rhs) const -> int {
        if (is_numeric() && rhs.is_numeric()) {
            double lhs_num = type == TYPE_INT ? static_cast<double>(int_val) : static_cast<double>(float_val);
            double rhs_num = rhs.type == TYPE_INT ? static_cast<double>(rhs.int_val) : static_cast<double>(rhs.float_val);
            if (lhs_num < rhs_num) {
                return -1;
            }
            if (lhs_num > rhs_num) {
                return 1;
            }
            return 0;
        }

        if (type == TYPE_STRING && rhs.type == TYPE_STRING) {
            if (str_val < rhs.str_val) {
                return -1;
            }
            if (str_val > rhs.str_val) {
                return 1;
            }
            return 0;
        }

        throw InternalError("Unexpected value type combination in Value::compare");
    }

    inline auto equals(const Value &rhs) const -> bool {
        return compare(rhs) == 0;
    }

    friend inline auto operator==(const Value &lhs, const Value &rhs) -> bool {
        return lhs.equals(rhs);
    }

    friend inline auto operator!=(const Value &lhs, const Value &rhs) -> bool {
        return !lhs.equals(rhs);
    }
};

/**
 * @brief Convert a literal/runtime value to the target column type before materialization.
 *
 * The only implicit conversion currently allowed for writes is INT -> FLOAT.
 * Other cross-type writes still fail to avoid silent truncation or lossy casts.
 */
inline auto coerce_value_to_type(const Value &src, ColType target_type) -> Value {
    Value coerced = src;
    // Force callers to rebuild raw bytes with the destination column length.
    coerced.raw.reset();
    if (src.type == target_type) {
        return coerced;
    }
    if (target_type == TYPE_FLOAT && src.type == TYPE_INT) {
        coerced.set_float(static_cast<float>(src.int_val));
        return coerced;
    }
    throw IncompatibleTypeError(coltype2str(target_type), coltype2str(src.type));
}

struct Condition {
    TabCol lhs_col;   // left-hand side column
    CompOp op;        // comparison operator
    bool is_rhs_val;  // true if right-hand side is a value (not a column)
    TabCol rhs_col;   // right-hand side column
    Value rhs_val;    // right-hand side value
    
    inline auto equals(const Condition &rhs) const -> bool {
        // 左列， 操作符， RHS种类&值
        if (!lhs_col.equals(rhs.lhs_col) || op != rhs.op || is_rhs_val != rhs.is_rhs_val) return false;
        if (is_rhs_val) {
            return rhs_val.equals(rhs.rhs_val);
        } 
        return rhs_col.equals(rhs.rhs_col);
    }
};

struct SetClause {
    TabCol lhs;
    Value rhs;
};

struct AggInfo {
    AggType agg_type;
    bool is_star;
    TabCol col;

    inline auto equals(const AggInfo &rhs) const -> bool {
        if (agg_type != rhs.agg_type || is_star != rhs.is_star) {
            return false;
        }
        if (is_star) {
            return true;
        }
        return col.equals(rhs.col);
    }
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
