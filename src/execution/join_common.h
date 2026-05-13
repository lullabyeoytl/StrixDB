#pragma once

#include <cstring>
#include <memory>
#include <vector>

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
