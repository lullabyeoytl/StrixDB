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

#include <vector>
#include <optional>


#include "transaction/transaction.h"
#include "transaction/transaction_manager.h"
#include "common/common.h"

auto ReconstructTuple(const TabMeta *schema, const RmRecord &base_tuple, const TupleMeta &base_meta,
                      const std::vector<UndoLog> &undo_logs) -> std::optional<RmRecord>;


auto IsWriteWriteConflict(timestamp_t tuple_ts, Transaction *txn) -> bool;

inline auto find_col_meta(const std::vector<ColMeta> &cols, const TabCol &target) -> const ColMeta & {
    auto it = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
        return col.tab_name == target.tab_name && col.name == target.col_name;
    });
    if (it == cols.end()) {
        throw InternalError("Analyze guaranteed column existence before execution");
    }
    return *it;
}

inline auto get_col_value(const RmRecord &record, const ColMeta &col) -> Value {
    Value value;
    const char *rec_buf = record.data + col.offset;
    if (col.type == TYPE_INT) {
        value.set_int(*reinterpret_cast<const int *>(rec_buf));
    } else if (col.type == TYPE_FLOAT) {
        value.set_float(*reinterpret_cast<const float *>(rec_buf));
    } else if (col.type == TYPE_STRING) {
        std::string str(rec_buf, col.len);
        str.resize(std::strlen(str.c_str()));
        value.set_str(str);
    } else {
        throw InternalError("Unexpected column type in get_col_value");
    }
    return value;
}

inline auto compare_values(const Value &lhs, const Value &rhs, CompOp op) -> bool {
    assert(lhs.type == rhs.type);
    switch (lhs.type) {
        case TYPE_INT: {
            switch (op) {
                case OP_EQ: return lhs.int_val == rhs.int_val;
                case OP_NE: return lhs.int_val != rhs.int_val;
                case OP_LT: return lhs.int_val < rhs.int_val;
                case OP_GT: return lhs.int_val > rhs.int_val;
                case OP_LE: return lhs.int_val <= rhs.int_val;
                case OP_GE: return lhs.int_val >= rhs.int_val;
            }
            break;
        }
        case TYPE_FLOAT: {
            // Strict arithmetic comparison. Type compatibility is guaranteed by Analyze::check_clause.
            switch (op) {
                case OP_EQ: return lhs.float_val == rhs.float_val;
                case OP_NE: return lhs.float_val != rhs.float_val;
                case OP_LT: return lhs.float_val < rhs.float_val;
                case OP_GT: return lhs.float_val > rhs.float_val;
                case OP_LE: return lhs.float_val <= rhs.float_val;
                case OP_GE: return lhs.float_val >= rhs.float_val;
            }
            break;
        }
        case TYPE_STRING: {
            switch (op) {
                case OP_EQ: return lhs.str_val == rhs.str_val;
                case OP_NE: return lhs.str_val != rhs.str_val;
                case OP_LT: return lhs.str_val < rhs.str_val;
                case OP_GT: return lhs.str_val > rhs.str_val;
                case OP_LE: return lhs.str_val <= rhs.str_val;
                case OP_GE: return lhs.str_val >= rhs.str_val;
            }
            break;
        }
        default:
            break;
    }
    throw InternalError("Unexpected value type in compare_values");
}

inline auto evaluate_condition(const Condition &cond, const RmRecord &record, const std::vector<ColMeta> &cols) -> bool {
    const auto &lhs_meta = find_col_meta(cols, cond.lhs_col);
    Value lhs_value = get_col_value(record, lhs_meta);
    Value rhs_value = cond.is_rhs_val ? cond.rhs_val : get_col_value(record, find_col_meta(cols, cond.rhs_col));
    return compare_values(lhs_value, rhs_value, cond.op);
}

inline auto evaluate_conditions(const std::vector<Condition> &conds, const RmRecord &record,
                                const std::vector<ColMeta> &cols) -> bool {
    for (const auto &cond : conds) {
        if (!evaluate_condition(cond, record, cols)) {
            return false;
        }
    }
    return true;
}