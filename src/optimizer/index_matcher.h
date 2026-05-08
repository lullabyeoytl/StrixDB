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
#include <optional>
#include <string>
#include <vector>

#include "common/common.h"
#include "system/sm_meta.h"

/**
 * @brief Struct representing the score of an index match without Statistics.
 * 1. **eq_prefix_len** — 按索引列序扫描，连续 EQ 匹配长度（越长越好）
 * 2. **has_range** — EQ 前缀之后的下一列是否有范围条件（有者优先）
 * 3. **index_width** — 索引总列数（越少越窄者优先，tiebreaker）
 */ 
struct IndexMatchScore {
    int eq_prefix_len = 0;
    bool has_range = false;
    int index_width = 0;

    auto better_than(const IndexMatchScore &other) const -> bool {
        if (eq_prefix_len != other.eq_prefix_len) {
            return eq_prefix_len > other.eq_prefix_len;
        }
        if (has_range != other.has_range) {
            return has_range;
        }
        return index_width < other.index_width;
    }
};

struct IndexMatchResult {
    bool matched = false;
    IndexMatchScore score;
    std::vector<Condition> lookup_conds;
    std::vector<Condition> residual_conds;
    std::vector<std::string> index_col_names;
    std::optional<IndexMeta> index_meta;
};

inline auto index_cond_matches_storage_type(const Condition &cond, const ColMeta &index_col) -> bool {
    if (!cond.is_rhs_val || !are_comparable_types(index_col.type, cond.rhs_val.type)) {
        return false;
    }
    return cond.rhs_val.type == index_col.type;
}

inline auto match_index(const IndexMeta &index_meta, const std::vector<Condition> &conds,
                        const std::vector<TabCol> &required_cols) -> IndexMatchResult {
    IndexMatchResult result;
    // Covering is intentionally excluded from scoring until the executor can avoid heap fetches.
    // TODO: covering index implementation
    (void)required_cols;
    result.index_col_names = index_meta.col_names();
    result.index_meta = index_meta;
    result.score.index_width = index_meta.col_num;
    
    std::vector<bool> selected(conds.size(), false);
    int matched_cols = 0;
    bool range_consumed = false;

    for (const auto &index_col : index_meta.cols) {
        if (range_consumed) {
            break;
        }

        int eq_idx = -1;
        std::vector<int> range_idxs;
        for (size_t i = 0; i < conds.size(); ++i) {
            const auto &cond = conds[i];
            if (!cond.is_rhs_val || cond.lhs_col.tab_name != index_meta.tab_name || cond.lhs_col.col_name != index_col.name) {
                continue;
            }
            if (!index_cond_matches_storage_type(cond, index_col)) {
                continue;
            }
            if (cond.op == OP_EQ && eq_idx == -1) {
                // first OP_EQ
                eq_idx = static_cast<int>(i);
            } else if (is_range_comp_op(cond.op)) {
                range_idxs.push_back(static_cast<int>(i));
            }
        }

        if (eq_idx != -1) {
            selected[eq_idx] = true;
            ++result.score.eq_prefix_len;
            ++matched_cols;
            continue;
        }

        if (!range_idxs.empty()) {
            for (int idx : range_idxs) {
                selected[idx] = true;
            }
            result.score.has_range = true;
            range_consumed = true;  // range_col following not used
            ++matched_cols;
        }

        break;
    }

    if (matched_cols == 0) {
        return result;
    }

    result.matched = true;
    for (size_t i = 0; i < conds.size(); ++i) {
        if (selected[i]) {
            result.lookup_conds.push_back(conds[i]);
        } else {
            result.residual_conds.push_back(conds[i]);
        }
    }
    return result;
}

inline auto match_best_index(const TabMeta &tab, const std::vector<Condition> &conds,
                             const std::vector<TabCol> &required_cols) -> IndexMatchResult {
    IndexMatchResult best;
    for (const auto &index_meta : tab.indexes) {
        auto current = match_index(index_meta, conds, required_cols);
        if (!current.matched) {
            continue;
        }
        if (!best.matched || current.score.better_than(best.score)) {
            best = std::move(current);
        }
    }
    return best;
}
