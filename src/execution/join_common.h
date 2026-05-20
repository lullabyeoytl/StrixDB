#pragma once

#include <cstring>
#include <memory>
#include <vector>

#include "execution_common.h"
#include "parser/ast.h"

struct JoinSchemaLayout {
    size_t left_len = 0;
    size_t right_len = 0;
    size_t eval_len = 0;
    std::vector<ColMeta> eval_cols;
    size_t output_len = 0;
    std::vector<ColMeta> output_cols;
};

inline auto build_join_eval_cols(const std::vector<ColMeta> &left_cols, const std::vector<ColMeta> &right_cols,
                                 size_t left_len) -> std::vector<ColMeta> {
    std::vector<ColMeta> eval_cols = left_cols;
    auto shifted_right_cols = right_cols;
    for (auto &col : shifted_right_cols) {
        col.offset += left_len;
    }
    eval_cols.insert(eval_cols.end(), shifted_right_cols.begin(), shifted_right_cols.end());
    return eval_cols;
}

inline auto build_join_schema_layout(const std::vector<ColMeta> &left_cols, size_t left_len,
                                     const std::vector<ColMeta> &right_cols, size_t right_len,
                                     JoinType join_type) -> JoinSchemaLayout {
    JoinSchemaLayout layout;
    layout.left_len = left_len;
    layout.right_len = right_len;
    layout.eval_len = left_len + right_len;
    layout.eval_cols = build_join_eval_cols(left_cols, right_cols, left_len);
    if (join_type == SEMI_JOIN) {
        layout.output_len = left_len;
        layout.output_cols = left_cols;
    } else {
        layout.output_len = layout.eval_len;
        layout.output_cols = layout.eval_cols;
    }
    return layout;
}

inline auto build_join_eval_record(const RmRecord &left_rec, const RmRecord &right_rec, size_t left_len,
                                   size_t right_len) -> std::unique_ptr<RmRecord> {
    auto out = std::make_unique<RmRecord>(static_cast<int>(left_len + right_len));
    memcpy(out->data, left_rec.data, left_len);
    memcpy(out->data + left_len, right_rec.data, right_len);
    return out;
}

inline auto build_semi_output_record(const RmRecord &left_rec, size_t left_len) -> std::unique_ptr<RmRecord> {
    auto out = std::make_unique<RmRecord>(static_cast<int>(left_len));
    memcpy(out->data, left_rec.data, left_len);
    return out;
}

inline auto extract_join_key_values(const RmRecord &record, const std::vector<ColMeta> &key_metas)
    -> std::vector<Value> {
    std::vector<Value> key_values;
    key_values.reserve(key_metas.size());
    for (const auto &meta : key_metas) {
        key_values.push_back(get_col_value(record, meta));
    }
    return key_values;
}

inline auto compare_join_keys(const RmRecord &left_rec, const std::vector<ColMeta> &left_key_metas,
                              const RmRecord &right_rec, const std::vector<ColMeta> &right_key_metas) -> int {
    auto left_key_values = extract_join_key_values(left_rec, left_key_metas);
    auto right_key_values = extract_join_key_values(right_rec, right_key_metas);
    for (size_t i = 0; i < left_key_values.size(); ++i) {
        int cmp = left_key_values[i].compare(right_key_values[i]);
        if (cmp != 0) {
            return cmp;
        }
    }
    return 0;
}

inline auto evaluate_join_pair_conditions(const std::vector<Condition> &conds, const RmRecord &left_rec,
                                          const RmRecord &right_rec, size_t left_len, size_t right_len,
                                          const std::vector<ColMeta> &eval_cols) -> bool {
    if (conds.empty()) {
        return true;
    }
    auto joined = build_join_eval_record(left_rec, right_rec, left_len, right_len);
    return evaluate_conditions(conds, *joined, eval_cols);
}
