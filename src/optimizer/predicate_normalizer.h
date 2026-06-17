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
#include <map>
#include <numeric>
#include <string>
#include <vector>

#include "common/common.h"
#include "system/sm_meta.h"

struct NormalizeResult {
    std::vector<Condition> normalized_conds;
    bool contradiction = false;
};


// given a column and a value, return a condition that is always true
inline auto normalize_make_const_condition(const TabCol &lhs_col, const Value &rhs_val,
                                           const std::vector<ColMeta> &cols) -> Condition {
    Condition inferred;
    const auto &lhs_meta = find_col_meta(cols, lhs_col);
    inferred.lhs_col = lhs_col;
    inferred.op = OP_EQ;
    inferred.is_rhs_val = true;
    inferred.rhs_val = coerce_value_to_type(rhs_val, lhs_meta.type, true);
    if (inferred.rhs_val.type == lhs_meta.type) {
        inferred.rhs_val.init_raw(lhs_meta.len);
    }
    return inferred;
}

inline auto normalize_predicates(const std::vector<ColMeta> &cols, const std::vector<Condition> &conds)
    -> NormalizeResult {
    NormalizeResult result;
    result.normalized_conds.reserve(conds.size());
    
    // step 1: collect conditions, then sort + unique to remove duplicates
    result.normalized_conds.assign(conds.begin(), conds.end());
    std::sort(result.normalized_conds.begin(), result.normalized_conds.end(), condition_less);
    result.normalized_conds.erase(
        std::unique(result.normalized_conds.begin(), result.normalized_conds.end(),
                    [](const Condition &a, const Condition &b) { return a.equals(b); }),
        result.normalized_conds.end());
    
    // step 2: handle equivalence class
    std::vector<TabCol> columns;
    std::map<std::string, int> column_ids;
    std::vector<int> parent;
    std::vector<int> sz;  // union by size: track component sizes

    // col_name -> int index key
    auto ensure_column = [&](const TabCol &col) {
        std::string key = col.tab_name + "." + col.col_name;
        auto [it, inserted] = column_ids.emplace(key, static_cast<int>(columns.size()));
        if (inserted) {
            columns.push_back(col);
            parent.push_back(static_cast<int>(parent.size()));
            sz.push_back(1);
        }
        return it->second;
    };
    
    for (const auto &cond : result.normalized_conds) {
        if (cond.op != OP_EQ) {
            continue;
        }
        ensure_column(cond.lhs_col);
        if (!cond.is_rhs_val) {
            ensure_column(cond.rhs_col);
        }
    }
    
    // set union
    auto find_root = [&](int x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    auto unite = [&](int lhs, int rhs) {
        lhs = find_root(lhs);
        rhs = find_root(rhs);
        if (lhs == rhs) {
            return;
        }
        // Union by size: attach smaller tree under larger tree
        if (sz[lhs] < sz[rhs]) {
            std::swap(lhs, rhs);
        }
        parent[rhs] = lhs;
        sz[lhs] += sz[rhs];
    };

    for (const auto &cond : result.normalized_conds) {
        if (cond.op == OP_EQ && !cond.is_rhs_val) {
            unite(ensure_column(cond.lhs_col), ensure_column(cond.rhs_col));
        }
    }
    
    // step 3, check for contradictions
    std::map<int, Value> component_constants;
    for (const auto &cond : result.normalized_conds) {
        if (cond.op != OP_EQ || !cond.is_rhs_val) {
            continue;
        }
        int root = find_root(ensure_column(cond.lhs_col));
        auto [it, inserted] = component_constants.emplace(root, cond.rhs_val);
        if (!inserted && !it->second.equals(cond.rhs_val)) {
            result.contradiction = true;
            return result;
        }
    }
    
    // step 4, spread const val for columns (defer duplicate removal to final pass)
    for (const auto &col : columns) {
        int root = find_root(ensure_column(col));
        auto const_it = component_constants.find(root);
        if (const_it == component_constants.end()) {
            continue;
        }
        result.normalized_conds.push_back(
            normalize_make_const_condition(col, const_it->second, cols));
    }

    // Final dedup after inferred conditions have been added
    std::sort(result.normalized_conds.begin(), result.normalized_conds.end(), condition_less);
    result.normalized_conds.erase(
        std::unique(result.normalized_conds.begin(), result.normalized_conds.end(),
                    [](const Condition &a, const Condition &b) { return a.equals(b); }),
        result.normalized_conds.end());

    return result;
}
