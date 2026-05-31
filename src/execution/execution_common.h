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


#include "transaction/transaction.h"
#include "transaction/transaction_manager.h"
#include "common/common.h"


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
    } else if (col.type == TYPE_DATETIME) {
        value.set_datetime(*reinterpret_cast<const int64_t *>(rec_buf));
    } else {
        throw InternalError("Unexpected column type in get_col_value");
    }
    return value;
}

inline auto compare_values(const Value &lhs, const Value &rhs, CompOp op) -> bool {
    return compare_result_matches_op(lhs.compare(rhs), op);
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

template <typename RecordFetcher>
inline auto seek_to_next_valid_tuple(RecScan *scan, Rid &rid, const std::vector<Condition> &conds,
                                     const std::vector<ColMeta> &cols, RecordFetcher &&fetch_record) -> bool {
    if (scan == nullptr || scan->is_end()) {
        rid = Rid{-1, -1};
        return false;
    }

    while (!scan->is_end()) {
        rid = scan->rid();
        auto record = fetch_record(rid);
        if (evaluate_conditions(conds, *record, cols)) {
            return true;
        }
        scan->next();
    }

    rid = Rid{-1, -1};
    return false;
}
