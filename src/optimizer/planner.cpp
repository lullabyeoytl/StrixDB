/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "planner.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <set>

#include "execution/executor_delete.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_insert.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_update.h"
#include "index/ix.h"
#include "index_matcher.h"
#include "predicate_normalizer.h"
#include "record_printer.h"

namespace {

struct JoinSubtree {
    std::shared_ptr<Plan> plan;
    std::set<std::string> tables;
};

struct LeafPlanShape {
    std::shared_ptr<ProjectionPlan> projection;
    std::shared_ptr<FilterPlan> filter;
    std::shared_ptr<ScanPlan> scan;
};

std::string ast_col_to_string(const std::shared_ptr<ast::Col> &col) {
    return col->tab_name.empty() ? col->col_name : col->tab_name + "." + col->col_name;
}

std::string tab_col_to_string(const TabCol &col) {
    return col.tab_name.empty() ? col.col_name : col.tab_name + "." + col.col_name;
}

std::string ast_value_to_string(const std::shared_ptr<ast::Value> &value) {
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(value)) {
        return std::to_string(int_lit->val);
    }
    if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(value)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.6f", float_lit->val);
        return buf;
    }
    if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(value)) {
        return "'" + str_lit->val + "'";
    }
    if (auto bool_lit = std::dynamic_pointer_cast<ast::BoolLit>(value)) {
        return bool_lit->val ? "1" : "0";
    }
    return "";
}

std::string ast_op_to_string(ast::SvCompOp op) {
    switch (op) {
        case ast::SV_OP_EQ: return "=";
        case ast::SV_OP_NE: return "<>";
        case ast::SV_OP_LT: return "<";
        case ast::SV_OP_GT: return ">";
        case ast::SV_OP_LE: return "<=";
        case ast::SV_OP_GE: return ">=";
    }
    return "";
}

std::string ast_cond_to_string(const std::shared_ptr<ast::BinaryExpr> &cond) {
    std::string rhs;
    if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs)) {
        rhs = ast_col_to_string(rhs_col);
    } else if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(cond->rhs)) {
        rhs = ast_value_to_string(rhs_val);
    }
    return ast_col_to_string(cond->lhs) + ast_op_to_string(cond->op) + rhs;
}

std::string sorted_join_string(std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += values[i];
    }
    return out;
}

std::string join_tables_string(const std::set<std::string> &tables) {
    std::string out;
    bool first = true;
    for (const auto &table : tables) {
        if (!first) {
            out += ", ";
        }
        out += table;
        first = false;
    }
    return out;
}

bool table_has_filter(const std::vector<std::shared_ptr<ast::BinaryExpr>> &conds, const std::string &visible) {
    for (const auto &cond : conds) {
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (cond->lhs->tab_name == visible &&
            (std::dynamic_pointer_cast<ast::Value>(cond->rhs) || (rhs_col && rhs_col->tab_name == visible))) {
            return true;
        }
    }
    return false;
}

std::string ast_col_key(const std::shared_ptr<ast::Col> &col) {
    return col->tab_name + "\x1f" + col->col_name;
}

void propagate_equal_join_value_filters(std::vector<std::shared_ptr<ast::BinaryExpr>> &conds) {
    std::map<std::string, std::shared_ptr<ast::Col>> key_to_col;
    std::map<std::string, std::vector<std::string>> graph;
    for (const auto &cond : conds) {
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (!rhs_col || cond->op != ast::SV_OP_EQ || cond->lhs->tab_name == rhs_col->tab_name) {
            continue;
        }
        std::string lhs_key = ast_col_key(cond->lhs);
        std::string rhs_key = ast_col_key(rhs_col);
        key_to_col[lhs_key] = cond->lhs;
        key_to_col[rhs_key] = rhs_col;
        graph[lhs_key].push_back(rhs_key);
        graph[rhs_key].push_back(lhs_key);
    }

    std::map<std::string, std::vector<std::shared_ptr<ast::Col>>> component_cols;
    std::set<std::string> visited;
    for (const auto &entry : key_to_col) {
        if (!visited.insert(entry.first).second) {
            continue;
        }
        std::vector<std::string> stack{entry.first};
        std::vector<std::shared_ptr<ast::Col>> cols;
        while (!stack.empty()) {
            std::string current = stack.back();
            stack.pop_back();
            cols.push_back(key_to_col.at(current));
            for (const auto &next : graph[current]) {
                if (visited.insert(next).second) {
                    stack.push_back(next);
                }
            }
        }
        for (const auto &col : cols) {
            component_cols[ast_col_key(col)] = cols;
        }
    }

    std::set<std::string> existing;
    for (const auto &cond : conds) {
        existing.insert(ast_cond_to_string(cond));
    }
    std::vector<std::shared_ptr<ast::BinaryExpr>> additions;
    for (const auto &cond : conds) {
        if (!std::dynamic_pointer_cast<ast::Value>(cond->rhs)) {
            continue;
        }
        auto comp_it = component_cols.find(ast_col_key(cond->lhs));
        if (comp_it == component_cols.end()) {
            continue;
        }
        for (const auto &target_col : comp_it->second) {
            if (target_col->tab_name == cond->lhs->tab_name && target_col->col_name == cond->lhs->col_name) {
                continue;
            }
            auto derived_lhs = std::make_shared<ast::Col>(target_col->tab_name, target_col->col_name);
            auto derived = std::make_shared<ast::BinaryExpr>(derived_lhs, cond->op, cond->rhs);
            if (existing.insert(ast_cond_to_string(derived)).second) {
                additions.push_back(std::move(derived));
            }
        }
    }
    conds.insert(conds.end(), additions.begin(), additions.end());
}

std::string table_filter_string(const std::vector<std::shared_ptr<ast::BinaryExpr>> &conds, const std::string &visible) {
    std::vector<std::string> filters;
    for (const auto &cond : conds) {
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (cond->lhs->tab_name == visible &&
            (std::dynamic_pointer_cast<ast::Value>(cond->rhs) || (rhs_col && rhs_col->tab_name == visible))) {
            filters.push_back(ast_cond_to_string(cond));
        }
    }
    return sorted_join_string(std::move(filters));
}

std::vector<std::string> make_explain_lines(const std::shared_ptr<Query> &query,
                                            const std::shared_ptr<ast::SelectStmt> &select,
                                            SmManager *sm_manager) {
    auto visible_name = [](const std::shared_ptr<ast::TableRef> &ref) {
        return ref->alias.empty() ? ref->name : ref->alias;
    };

    std::map<std::string, std::string> alias_to_table = query->table_storage_names;
    std::vector<std::string> input_visible;
    for (const auto &ref : select->table_refs) {
        std::string visible = visible_name(ref);
        alias_to_table[visible] = ref->name;
        input_visible.push_back(visible);
    }

    std::vector<std::shared_ptr<ast::BinaryExpr>> explain_conds = select->conds;
    for (const auto &join_expr : select->jointree) {
        explain_conds.insert(explain_conds.end(), join_expr->conds.begin(), join_expr->conds.end());
    }
    propagate_equal_join_value_filters(explain_conds);

    auto table_rows = [&](const std::string &alias) {
        int rows = 0;
        auto fh = sm_manager->fhs_.at(alias_to_table[alias]).get();
        for (RmScan scan(fh); !scan.is_end(); scan.next()) {
            rows++;
        }
        return rows;
    };

    auto table_has_join_index = [&](const std::string &alias, const std::shared_ptr<ast::BinaryExpr> &cond) {
        std::vector<std::string> cols;
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (cond->lhs->tab_name == alias) {
            cols.push_back(cond->lhs->col_name);
        } else if (rhs_col && rhs_col->tab_name == alias) {
            cols.push_back(rhs_col->col_name);
        }
        if (cols.empty()) {
            return false;
        }
        return sm_manager->db_.get_table(alias_to_table[alias]).is_index(cols);
    };

    auto read_col = [&](const std::string &alias, const Rid &rid, const std::string &col_name) {
        auto &tab = sm_manager->db_.get_table(alias_to_table[alias]);
        auto col = tab.get_col(col_name);
        auto rec = sm_manager->fhs_.at(alias_to_table[alias])->get_record(rid, nullptr);
        std::string raw(rec->data + col->offset, col->len);
        return std::pair<ColMeta, std::string>(*col, raw);
    };

    auto compare_raw = [&](const ColMeta &lhs_col, const std::string &lhs_raw,
                           const ColMeta &rhs_col, const std::string &rhs_raw,
                           ast::SvCompOp op) {
        int cmp = 0;
        if (lhs_col.type == TYPE_FLOAT || rhs_col.type == TYPE_FLOAT) {
            float lhs = lhs_col.type == TYPE_FLOAT ? *reinterpret_cast<const float *>(lhs_raw.data())
                                                   : static_cast<float>(*reinterpret_cast<const int *>(lhs_raw.data()));
            float rhs = rhs_col.type == TYPE_FLOAT ? *reinterpret_cast<const float *>(rhs_raw.data())
                                                   : static_cast<float>(*reinterpret_cast<const int *>(rhs_raw.data()));
            cmp = (lhs > rhs) - (lhs < rhs);
        } else if (lhs_col.type == TYPE_INT) {
            int lhs = *reinterpret_cast<const int *>(lhs_raw.data());
            int rhs = *reinterpret_cast<const int *>(rhs_raw.data());
            cmp = (lhs > rhs) - (lhs < rhs);
        } else {
            std::string lhs = lhs_raw;
            std::string rhs = rhs_raw;
            lhs.resize(strlen(lhs.c_str()));
            rhs.resize(strlen(rhs.c_str()));
            cmp = lhs.compare(rhs);
        }
        switch (op) {
            case ast::SV_OP_EQ: return cmp == 0;
            case ast::SV_OP_NE: return cmp != 0;
            case ast::SV_OP_LT: return cmp < 0;
            case ast::SV_OP_GT: return cmp > 0;
            case ast::SV_OP_LE: return cmp <= 0;
            case ast::SV_OP_GE: return cmp >= 0;
        }
        return false;
    };

    auto compare_value_cond = [&](const std::string &alias, const Rid &rid,
                                  const std::shared_ptr<ast::BinaryExpr> &cond) {
        auto [lhs_col, lhs_raw] = read_col(alias, rid, cond->lhs->col_name);
        ColMeta rhs_col = lhs_col;
        std::string rhs_raw(lhs_col.len, '\0');
        if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(cond->rhs)) {
            int value = int_lit->val;
            if (lhs_col.type == TYPE_FLOAT) {
                float f = static_cast<float>(value);
                memcpy(rhs_raw.data(), &f, sizeof(float));
                rhs_col.type = TYPE_FLOAT;
                rhs_col.len = sizeof(float);
            } else {
                memcpy(rhs_raw.data(), &value, sizeof(int));
                rhs_col.type = TYPE_INT;
                rhs_col.len = sizeof(int);
            }
        } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(cond->rhs)) {
            float value = float_lit->val;
            memcpy(rhs_raw.data(), &value, sizeof(float));
            rhs_col.type = TYPE_FLOAT;
            rhs_col.len = sizeof(float);
        } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(cond->rhs)) {
            rhs_raw.assign(lhs_col.len, '\0');
            memcpy(rhs_raw.data(), str_lit->val.c_str(), std::min<int>(lhs_col.len, str_lit->val.size()));
            rhs_col.type = TYPE_STRING;
        } else if (auto bool_lit = std::dynamic_pointer_cast<ast::BoolLit>(cond->rhs)) {
            int value = bool_lit->val ? 1 : 0;
            memcpy(rhs_raw.data(), &value, sizeof(int));
            rhs_col.type = TYPE_INT;
            rhs_col.len = sizeof(int);
        }
        return compare_raw(lhs_col, lhs_raw, rhs_col, rhs_raw, cond->op);
    };

    std::map<std::string, std::vector<std::shared_ptr<ast::BinaryExpr>>> local_conds;
    std::vector<std::shared_ptr<ast::BinaryExpr>> join_conds;
    for (const auto &cond : explain_conds) {
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (rhs_col && rhs_col->tab_name != cond->lhs->tab_name) {
            join_conds.push_back(cond);
        } else {
            local_conds[cond->lhs->tab_name].push_back(cond);
        }
    }

    auto compare_filter_cond = [&](const std::string &alias, const Rid &rid,
                                   const std::shared_ptr<ast::BinaryExpr> &cond) {
        if (std::dynamic_pointer_cast<ast::Value>(cond->rhs)) {
            return compare_value_cond(alias, rid, cond);
        }
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        auto [lhs_col, lhs_raw] = read_col(alias, rid, cond->lhs->col_name);
        auto [rhs_meta, rhs_raw] = read_col(alias, rid, rhs_col->col_name);
        return compare_raw(lhs_col, lhs_raw, rhs_meta, rhs_raw, cond->op);
    };

    auto filtered_rows = [&](const std::string &alias) {
        int rows = 0;
        auto fh = sm_manager->fhs_.at(alias_to_table[alias]).get();
        for (RmScan scan(fh); !scan.is_end(); scan.next()) {
            bool pass = true;
            for (const auto &cond : local_conds[alias]) {
                if (!compare_filter_cond(alias, scan.rid(), cond)) {
                    pass = false;
                    break;
                }
            }
            if (pass) {
                rows++;
            }
        }
        return rows;
    };

    auto join_count = [&](const std::set<std::string> &aliases) {
        std::vector<std::string> order;
        for (const auto &alias : input_visible) {
            if (aliases.count(alias)) {
                order.push_back(alias);
            }
        }
        std::map<std::string, Rid> current;
        int rows = 0;
        std::function<void(size_t)> dfs = [&](size_t idx) {
            if (idx == order.size()) {
                for (const auto &cond : explain_conds) {
                    auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
                    if (rhs_col) {
                        if (!aliases.count(cond->lhs->tab_name) || !aliases.count(rhs_col->tab_name)) {
                            continue;
                        }
                        auto [lhs_col, lhs_raw] = read_col(cond->lhs->tab_name, current[cond->lhs->tab_name],
                                                           cond->lhs->col_name);
                        auto [rhs_meta, rhs_raw] = read_col(rhs_col->tab_name, current[rhs_col->tab_name],
                                                            rhs_col->col_name);
                        if (!compare_raw(lhs_col, lhs_raw, rhs_meta, rhs_raw, cond->op)) {
                            return;
                        }
                    } else if (aliases.count(cond->lhs->tab_name) &&
                               !compare_filter_cond(cond->lhs->tab_name, current[cond->lhs->tab_name], cond)) {
                        return;
                    }
                }
                rows++;
                return;
            }
            auto &alias = order[idx];
            auto fh = sm_manager->fhs_.at(alias_to_table[alias]).get();
            for (RmScan scan(fh); !scan.is_end(); scan.next()) {
                current[alias] = scan.rid();
                dfs(idx + 1);
            }
        };
        dfs(0);
        return rows;
    };

    auto prefix_records = [&](const std::set<std::string> &aliases) {
        std::vector<std::string> order;
        for (const auto &alias : input_visible) {
            if (aliases.count(alias)) {
                order.push_back(alias);
            }
        }
        std::vector<std::map<std::string, Rid>> records;
        std::map<std::string, Rid> current;
        std::function<void(size_t)> dfs = [&](size_t idx) {
            if (idx == order.size()) {
                for (const auto &cond : explain_conds) {
                    auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
                    if (rhs_col) {
                        if (!aliases.count(cond->lhs->tab_name) || !aliases.count(rhs_col->tab_name)) {
                            continue;
                        }
                        auto [lhs_col, lhs_raw] = read_col(cond->lhs->tab_name, current[cond->lhs->tab_name],
                                                           cond->lhs->col_name);
                        auto [rhs_meta, rhs_raw] = read_col(rhs_col->tab_name, current[rhs_col->tab_name],
                                                            rhs_col->col_name);
                        if (!compare_raw(lhs_col, lhs_raw, rhs_meta, rhs_raw, cond->op)) {
                            return;
                        }
                    } else if (aliases.count(cond->lhs->tab_name) &&
                               !compare_filter_cond(cond->lhs->tab_name, current[cond->lhs->tab_name], cond)) {
                        return;
                    }
                }
                records.push_back(current);
                return;
            }
            auto &alias = order[idx];
            auto fh = sm_manager->fhs_.at(alias_to_table[alias]).get();
            for (RmScan scan(fh); !scan.is_end(); scan.next()) {
                current[alias] = scan.rid();
                dfs(idx + 1);
            }
        };
        dfs(0);
        return records;
    };

    auto find_index_cond = [&](const std::string &right_alias,
                               const std::vector<std::shared_ptr<ast::BinaryExpr>> &conds)
        -> std::shared_ptr<ast::BinaryExpr> {
        for (const auto &cond : conds) {
            if (cond->op == ast::SV_OP_EQ && table_has_join_index(right_alias, cond)) {
                return cond;
            }
        }
        return nullptr;
    };

    auto index_hit_count = [&](const std::set<std::string> &outer_aliases, const std::string &right_alias,
                               const std::shared_ptr<ast::BinaryExpr> &index_cond) {
        if (index_cond == nullptr) {
            return 0;
        }
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(index_cond->rhs);
        std::shared_ptr<ast::Col> right_col;
        std::shared_ptr<ast::Col> outer_col;
        if (index_cond->lhs->tab_name == right_alias && rhs_col && outer_aliases.count(rhs_col->tab_name)) {
            right_col = index_cond->lhs;
            outer_col = rhs_col;
        } else if (rhs_col && rhs_col->tab_name == right_alias && outer_aliases.count(index_cond->lhs->tab_name)) {
            right_col = rhs_col;
            outer_col = index_cond->lhs;
        } else {
            return 0;
        }

        int rows = 0;
        auto prefixes = prefix_records(outer_aliases);
        auto right_fh = sm_manager->fhs_.at(alias_to_table[right_alias]).get();
        for (const auto &prefix : prefixes) {
            auto [outer_meta, outer_raw] = read_col(outer_col->tab_name, prefix.at(outer_col->tab_name),
                                                    outer_col->col_name);
            for (RmScan scan(right_fh); !scan.is_end(); scan.next()) {
                auto [right_meta, right_raw] = read_col(right_alias, scan.rid(), right_col->col_name);
                if (!compare_raw(right_meta, right_raw, outer_meta, outer_raw, ast::SV_OP_EQ)) {
                    continue;
                }
                bool pass_local = true;
                for (const auto &cond : local_conds[right_alias]) {
                    if (!compare_filter_cond(right_alias, scan.rid(), cond)) {
                        pass_local = false;
                        break;
                    }
                }
                if (pass_local) {
                    rows++;
                }
            }
        }
        return rows;
    };

    std::vector<std::string> lines;
    std::set<std::string> all_aliases(input_visible.begin(), input_visible.end());
    int project_rows = input_visible.empty() ? 0 : join_count(all_aliases);
    if (query->display_wildcard) {
        lines.push_back("Project(columns=[*], rows=" + std::to_string(project_rows) + ")");
    } else {
        std::vector<std::string> cols;
        for (const auto &col : query->select_items) {
            cols.push_back(tab_col_to_string(col));
        }
        lines.push_back("Project(columns=[" + sorted_join_string(cols) + "], rows=" + std::to_string(project_rows) + ")");
    }

    std::set<std::string> joined_aliases;
    std::map<std::string, int> leaf_multiplier;
    std::map<std::string, int> leaf_output_rows;
    std::map<std::string, std::shared_ptr<ast::BinaryExpr>> leaf_index_cond;
    std::map<std::string, bool> leaf_index;
    std::map<std::string, int> leaf_depth;
    std::vector<std::string> leaf_order;
    std::vector<std::string> join_lines;
    if (!join_conds.empty()) {
        struct JoinStep {
            std::set<std::string> aliases;
            std::vector<std::shared_ptr<ast::BinaryExpr>> conds;
        };
        std::vector<JoinStep> join_steps;

        int table_count = static_cast<int>(input_visible.size());
        if (!input_visible.empty()) {
            const std::string &first_alias = input_visible.front();
            leaf_multiplier[first_alias] = 1;
            leaf_output_rows[first_alias] = filtered_rows(first_alias);
            leaf_index[first_alias] = false;
            leaf_depth[first_alias] = table_count;
            leaf_order.push_back(first_alias);
            joined_aliases.insert(first_alias);
        }

        for (size_t i = 1; i < input_visible.size(); ++i) {
            const std::string &right_alias = input_visible[i];
            std::vector<std::shared_ptr<ast::BinaryExpr>> step_conds;
            for (const auto &cond : join_conds) {
                auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
                if (!rhs_col) {
                    continue;
                }
                bool lhs_right = cond->lhs->tab_name == right_alias && joined_aliases.count(rhs_col->tab_name);
                bool rhs_right = rhs_col->tab_name == right_alias && joined_aliases.count(cond->lhs->tab_name);
                if (lhs_right || rhs_right) {
                    step_conds.push_back(cond);
                }
            }
            int prefix_rows = join_count(joined_aliases);
            leaf_multiplier[right_alias] = prefix_rows;
            leaf_index_cond[right_alias] = find_index_cond(right_alias, step_conds);
            leaf_index[right_alias] = leaf_index_cond[right_alias] != nullptr;
            leaf_depth[right_alias] = table_count - static_cast<int>(i) + 1;
            if (leaf_index[right_alias]) {
                leaf_output_rows[right_alias] = index_hit_count(joined_aliases, right_alias, leaf_index_cond[right_alias]);
            } else {
                leaf_output_rows[right_alias] = filtered_rows(right_alias) * prefix_rows;
            }
            leaf_order.push_back(right_alias);
            joined_aliases.insert(right_alias);
            if (!step_conds.empty()) {
                join_steps.push_back({joined_aliases, step_conds});
            }
        }

        for (const auto &step : join_steps) {
            std::set<std::string> joined_tables;
            for (const auto &alias : step.aliases) {
                joined_tables.insert(alias_to_table[alias]);
            }
            std::vector<std::string> conds;
            for (const auto &cond : step.conds) {
                conds.push_back(ast_cond_to_string(cond));
            }
            int join_rows = join_count(step.aliases);
            join_lines.push_back("Join(tables=[" + join_tables_string(joined_tables) + "], condition=[" +
                                 sorted_join_string(conds) + "], rows=" + std::to_string(join_rows) + ")");
        }
        for (int i = static_cast<int>(join_lines.size()) - 1; i >= 0; --i) {
            int depth = static_cast<int>(join_lines.size()) - i;
            lines.push_back(std::string(depth, '\t') + join_lines[i]);
        }
    }

    bool has_filters = false;
    for (const auto &visible : input_visible) {
        has_filters = has_filters || table_has_filter(explain_conds, visible);
    }
    if (join_conds.empty() && has_filters) {
        std::sort(leaf_order.begin(), leaf_order.end(), [&](const std::string &lhs, const std::string &rhs) {
            return alias_to_table[lhs] < alias_to_table[rhs];
        });
    }
    for (const auto &visible : input_visible) {
        if (std::find(leaf_order.begin(), leaf_order.end(), visible) == leaf_order.end()) {
            leaf_order.push_back(visible);
        }
    }

    for (const auto &visible : leaf_order) {
        std::vector<std::string> projected;
        if (!query->display_wildcard) {
            for (const auto &col : query->select_items) {
                if (col.tab_name == visible) {
                    projected.push_back(tab_col_to_string(col));
                }
            }
            for (const auto &cond : join_conds) {
                auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
                if (cond->lhs->tab_name == visible) {
                    projected.push_back(ast_col_to_string(cond->lhs));
                }
                if (rhs_col && rhs_col->tab_name == visible) {
                    projected.push_back(ast_col_to_string(rhs_col));
                }
            }
            std::sort(projected.begin(), projected.end());
            projected.erase(std::unique(projected.begin(), projected.end()), projected.end());
            if (!projected.empty() && !join_conds.empty()) {
                int base_rows = leaf_output_rows.count(visible) ? leaf_output_rows[visible] : filtered_rows(visible);
                lines.push_back(std::string(leaf_depth[visible], '\t') + "Project(columns=[" +
                                sorted_join_string(projected) + "], rows=" + std::to_string(base_rows) + ")");
            }
        }

        std::string filter = table_filter_string(explain_conds, visible);
        if (!filter.empty()) {
            int filter_rows = filtered_rows(visible) * (join_conds.empty() ? 1 : leaf_multiplier[visible]);
            int filter_depth = join_conds.empty() ? 1 : leaf_depth[visible] + (projected.empty() ? 0 : 1);
            lines.push_back(std::string(filter_depth, '\t') + "Filter(condition=[" + filter +
                            "], rows=" + std::to_string(filter_rows) + ")");
        }

        int scan_rows = leaf_index[visible] ? leaf_output_rows[visible]
                                            : table_rows(visible) * (join_conds.empty() ? 1 : leaf_multiplier[visible]);
        std::string scan = "Scan(table=" + alias_to_table[visible] + ", type=" +
                           (leaf_index[visible] ? "IndexScan" : "SeqScan");
        if (leaf_index[visible]) {
            auto index_cond = leaf_index_cond[visible];
            auto rhs_col = std::dynamic_pointer_cast<ast::Col>(index_cond->rhs);
            if (index_cond->lhs->tab_name == visible) {
                scan += ", using_index=(" + index_cond->lhs->col_name + ")";
            } else if (rhs_col && rhs_col->tab_name == visible) {
                scan += ", using_index=(" + rhs_col->col_name + ")";
            }
        }
        scan += ", rows=" + std::to_string(scan_rows) + ")";
        int scan_depth = 0;
        if (join_conds.empty()) {
            scan_depth = filter.empty() ? 1 : 2;
        } else {
            scan_depth = leaf_depth[visible] + (projected.empty() ? 0 : 1) + (filter.empty() ? 0 : 1);
        }
        lines.push_back(std::string(scan_depth, '\t') + scan);
    }
    return lines;
}

class ColumnUsageCollector {
   public:
    void collect_col(const TabCol &col) {
        if (col.tab_name.empty() || col.col_name.empty()) {
            return;
        }
        auto &cols = table_cols_[col.tab_name];
        if (!contains_col(cols, col)) {
            cols.push_back(col);
        }
    }

    void collect_condition(const Condition &cond) {
        collect_col(cond.lhs_col);
        if (!cond.is_rhs_val) {
            collect_col(cond.rhs_col);
        }
    }

    void collect_conditions(const std::vector<Condition> &conds) {
        for (const auto &cond : conds) {
            collect_condition(cond);
        }
    }

    void collect_cols(const std::vector<TabCol> &cols) {
        for (const auto &col : cols) {
            collect_col(col);
        }
    }

    void collect_aggregates(const std::vector<AggInfo> &aggs) {
        for (const auto &agg : aggs) {
            if (!agg.is_star) {
                collect_col(agg.col);
            }
        }
    }

    void collect_having(const std::vector<HavingCond> &conds) {
        for (const auto &cond : conds) {
            if (cond.is_agg) {
                collect_aggregates(std::vector<AggInfo>{cond.agg});
            } else {
                collect_col(cond.col);
            }
        }
    }

    void collect_assignments(const std::vector<SetClause> &set_clauses) {
        for (const auto &set_clause : set_clauses) {
            collect_col(set_clause.lhs);
        }
    }

    std::vector<TabCol> columns_for_table(const std::string &table_name) const {
        auto it = table_cols_.find(table_name);
        if (it == table_cols_.end()) {
            return {};
        }
        return it->second;
    }

   private:
    std::map<std::string, std::vector<TabCol>> table_cols_;
};

bool contains_table(const JoinSubtree &subtree, const std::string &table_name) {
    return subtree.tables.find(table_name) != subtree.tables.end();
}

std::optional<size_t> find_subtree_index(const std::vector<JoinSubtree> &subtrees,
                                         const std::string &table_name) {
    for (size_t i = 0; i < subtrees.size(); ++i) {
        if (contains_table(subtrees[i], table_name)) {
            return i;
        }
    }
    return std::nullopt;
}

JoinSubtree take_subtree(std::vector<JoinSubtree> &subtrees, size_t index) {
    auto subtree = std::move(subtrees[index]);
    subtrees.erase(subtrees.begin() + static_cast<std::ptrdiff_t>(index));
    return subtree;
}

std::pair<JoinSubtree, JoinSubtree> take_subtree_pair(std::vector<JoinSubtree> &subtrees,
                                                      size_t left_idx, size_t right_idx) {
    if (left_idx < right_idx) {
        auto right = take_subtree(subtrees, right_idx);
        auto left = take_subtree(subtrees, left_idx);
        return {std::move(left), std::move(right)};
    }
    auto left = take_subtree(subtrees, left_idx);
    auto right = take_subtree(subtrees, right_idx);
    return {std::move(left), std::move(right)};
}

std::vector<JoinSubtree> build_join_subtrees(std::vector<std::pair<std::string, std::shared_ptr<Plan>>> &table_plans) {
    std::vector<JoinSubtree> subtrees;
    subtrees.reserve(table_plans.size());
    for (auto &[name, plan] : table_plans) {
        subtrees.push_back(JoinSubtree{std::move(plan), std::set<std::string>{name}});
    }
    return subtrees;
}

bool condition_spans_subtrees(const Condition &cond, const JoinSubtree &left, const JoinSubtree &right) {
    if (cond.is_rhs_val) {
        return false;
    }
    bool lhs_in_left = contains_table(left, cond.lhs_col.tab_name);
    bool lhs_in_right = contains_table(right, cond.lhs_col.tab_name);
    bool rhs_in_left = contains_table(left, cond.rhs_col.tab_name);
    bool rhs_in_right = contains_table(right, cond.rhs_col.tab_name);
    return (lhs_in_left && rhs_in_right) || (lhs_in_right && rhs_in_left);
}

bool condition_tables_in_subtree(const Condition &cond, const JoinSubtree &subtree) {
    if (cond.is_rhs_val) {
        return contains_table(subtree, cond.lhs_col.tab_name);
    }
    return contains_table(subtree, cond.lhs_col.tab_name) && contains_table(subtree, cond.rhs_col.tab_name);
}

Condition orient_condition_for_join(const Condition &cond, const JoinSubtree &left, const JoinSubtree &right) {
    auto oriented = cond;
    if (oriented.is_rhs_val) {
        return oriented;
    }
    if (contains_table(left, oriented.lhs_col.tab_name) && contains_table(right, oriented.rhs_col.tab_name)) {
        return oriented;
    }
    if (contains_table(right, oriented.lhs_col.tab_name) && contains_table(left, oriented.rhs_col.tab_name)) {
        std::swap(oriented.lhs_col, oriented.rhs_col);
        oriented.op = kSwapOp.at(oriented.op);
    }
    return oriented;
}

std::vector<Condition> collect_display_join_conditions(const std::vector<Condition> &direct_conds,
                                                       const std::vector<Condition> &pending_conds,
                                                       const JoinSubtree &left, const JoinSubtree &right) {
    std::vector<Condition> display_conds;
    for (const auto &cond : direct_conds) {
        if (condition_spans_subtrees(cond, left, right)) {
            display_conds.push_back(cond);
        }
    }
    for (const auto &cond : pending_conds) {
        if (condition_spans_subtrees(cond, left, right)) {
            display_conds.push_back(cond);
        }
    }
    return display_conds;
}

auto inspect_leaf_plan(const std::shared_ptr<Plan> &plan) -> std::optional<LeafPlanShape> {
    LeafPlanShape shape;
    std::shared_ptr<Plan> current = plan;
    if (auto projection = std::dynamic_pointer_cast<ProjectionPlan>(current)) {
        shape.projection = projection;
        current = projection->subplan_;
    }
    if (auto filter = std::dynamic_pointer_cast<FilterPlan>(current)) {
        shape.filter = filter;
        current = filter->subplan_;
    }
    auto scan = std::dynamic_pointer_cast<ScanPlan>(current);
    if (scan == nullptr) {
        return std::nullopt;
    }
    shape.scan = scan;
    return shape;
}

auto rebuild_leaf_plan(const LeafPlanShape &shape, std::shared_ptr<ScanPlan> scan) -> std::shared_ptr<Plan> {
    std::shared_ptr<Plan> node = std::move(scan);
    if (shape.filter != nullptr) {
        node = std::make_shared<FilterPlan>(std::move(node), shape.filter->conds_);
    }
    if (shape.projection != nullptr) {
        node = std::make_shared<ProjectionPlan>(T_Projection, std::move(node), shape.projection->sel_cols_);
    }
    return node;
}

auto match_join_index(const TabMeta &tab, const std::string &right_visible, const std::vector<Condition> &join_conds)
    -> IndexMatchResult {
    IndexMatchResult best;
    for (const auto &index_meta : tab.indexes) {
        IndexMatchResult current;
        current.index_col_names = index_meta.col_names();
        current.index_meta = index_meta;
        current.score.index_width = index_meta.col_num;
        std::vector<bool> selected(join_conds.size(), false);
        for (const auto &index_col : index_meta.cols) {
            int eq_idx = -1;
            for (size_t i = 0; i < join_conds.size(); ++i) {
                const auto &cond = join_conds[i];
                if (cond.is_rhs_val || cond.op != OP_EQ) {
                    continue;
                }
                if (cond.rhs_col.tab_name == right_visible && cond.rhs_col.col_name == index_col.name) {
                    eq_idx = static_cast<int>(i);
                    break;
                }
            }
            if (eq_idx == -1) {
                break;
            }
            selected[eq_idx] = true;
            ++current.score.eq_prefix_len;
        }
        if (current.score.eq_prefix_len == 0) {
            continue;
        }
        current.matched = true;
        for (size_t i = 0; i < join_conds.size(); ++i) {
            if (selected[i]) {
                current.lookup_conds.push_back(join_conds[i]);
            } else {
                current.residual_conds.push_back(join_conds[i]);
            }
        }
        if (!best.matched || current.score.better_than(best.score)) {
            best = std::move(current);
        }
    }
    return best;
}

void parameterize_right_subtree_with_join_index(SmManager *sm_manager, JoinSubtree &right,
                                                const std::vector<Condition> &join_conds) {
    if (right.tables.size() != 1) {
        return;
    }
    auto leaf = inspect_leaf_plan(right.plan);
    if (!leaf.has_value() || leaf->scan == nullptr || leaf->scan->tag != T_SeqScan) {
        return;
    }
    if (leaf->scan->empty_result_) {
        return;
    }
    auto right_visible = *right.tables.begin();
    const auto &tab = sm_manager->db_.get_table(leaf->scan->tab_name_);
    auto match = match_join_index(tab, right_visible, join_conds);
    if (!match.matched) {
        return;
    }
    auto scan = std::make_shared<ScanPlan>(sm_manager, leaf->scan->tab_name_, std::move(match.lookup_conds),
                                           std::vector<Condition>(), std::move(match.index_col_names),
                                           std::move(match.index_meta), leaf->scan->visible_name_);
    scan->empty_result_ = leaf->scan->empty_result_;
    right.plan = rebuild_leaf_plan(*leaf, std::move(scan));
}

void bind_condition_to_subtree(JoinSubtree &subtree, Condition cond) {
    auto join = std::dynamic_pointer_cast<JoinPlan>(subtree.plan);
    if (join == nullptr) {
        throw InternalError("Join predicate does not span join inputs");
    }
    join->conds_.push_back(std::move(cond));
}

std::vector<Condition> collect_join_conditions(std::vector<Condition> direct_conds,
                                               std::vector<Condition> &pending_conds,
                                               JoinSubtree &left, JoinSubtree &right) {
    std::vector<Condition> join_conds;
    for (const auto &cond : direct_conds) {
        if (condition_spans_subtrees(cond, left, right)) {
            join_conds.push_back(orient_condition_for_join(cond, left, right));
        } else if (condition_tables_in_subtree(cond, left)) {
            bind_condition_to_subtree(left, cond);
        } else if (condition_tables_in_subtree(cond, right)) {
            bind_condition_to_subtree(right, cond);
        } else {
            throw InternalError("Join predicate does not belong to join inputs");
        }
    }

    auto it = pending_conds.begin();
    while (it != pending_conds.end()) {
        if (condition_spans_subtrees(*it, left, right)) {
            join_conds.push_back(orient_condition_for_join(*it, left, right));
            it = pending_conds.erase(it);
        } else {
            ++it;
        }
    }
    return join_conds;
}

JoinSubtree make_join_subtree(SmManager *sm_manager, JoinSubtree left, JoinSubtree right,
                              std::vector<Condition> conds, JoinType join_type,
                              std::vector<Condition> display_conds = {}) {
    auto tables = std::move(left.tables);
    tables.insert(right.tables.begin(), right.tables.end());
    PlanTag join_tag = T_NestLoop;
    std::vector<std::string> index_col_names;
    auto leaf = inspect_leaf_plan(right.plan);
    if (right.tables.size() == 1 && leaf.has_value() && leaf->scan != nullptr && leaf->scan->tag == T_SeqScan) {
        auto right_visible = *right.tables.begin();
        const auto &tab = sm_manager->db_.get_table(leaf->scan->tab_name_);
        auto match = match_join_index(tab, right_visible, conds);
        if (match.matched && !leaf->scan->empty_result_) {
            join_tag = T_IndexNestLoop;
            index_col_names = match.index_col_names;
            if (leaf->filter != nullptr && index_col_names.size() > 1) {
                conds.insert(conds.end(), leaf->filter->conds_.begin(), leaf->filter->conds_.end());
            }
            right.plan = leaf->scan;
        } else {
            parameterize_right_subtree_with_join_index(sm_manager, right, conds);
        }
    }
    auto plan = std::make_shared<JoinPlan>(join_tag, std::move(left.plan), std::move(right.plan),
                                           std::move(conds), join_type, false, std::move(display_conds),
                                           std::move(index_col_names));
    return JoinSubtree{std::move(plan), std::move(tables)};
}

std::optional<std::pair<size_t, size_t>> find_pending_join_subtrees(
    const std::vector<JoinSubtree> &subtrees, const Condition &cond) {
    if (cond.is_rhs_val) {
        return std::nullopt;
    }
    auto lhs_idx = find_subtree_index(subtrees, cond.lhs_col.tab_name);
    auto rhs_idx = find_subtree_index(subtrees, cond.rhs_col.tab_name);
    if (!lhs_idx.has_value() || !rhs_idx.has_value() || lhs_idx == rhs_idx) {
        return std::nullopt;
    }
    if (*lhs_idx < *rhs_idx) {
        return std::pair<size_t, size_t>{*lhs_idx, *rhs_idx};
    }
    return std::pair<size_t, size_t>{*rhs_idx, *lhs_idx};
}

bool join_from_pending_conditions(SmManager *sm_manager, std::vector<JoinSubtree> &subtrees,
                                  std::vector<Condition> &pending_conds) {
    std::optional<std::pair<size_t, size_t>> selected_indexes;
    for (size_t i = 0; i < pending_conds.size(); ++i) {
        auto indexes = find_pending_join_subtrees(subtrees, pending_conds[i]);
        if (indexes.has_value()) {
            selected_indexes = indexes;
            break;
        }
    }
    if (!selected_indexes.has_value()) {
        return false;
    }
    auto [left_idx, right_idx] = *selected_indexes;
    auto [left, right] = take_subtree_pair(subtrees, left_idx, right_idx);
    auto display_conds = collect_display_join_conditions(std::vector<Condition>(), pending_conds, left, right);
    auto join_conds = collect_join_conditions(std::vector<Condition>(), pending_conds, left, right);
    subtrees.push_back(make_join_subtree(sm_manager, std::move(left), std::move(right), std::move(join_conds),
                                         INNER_JOIN, std::move(display_conds)));
    return true;
}

auto convert_join_expr_conds(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds) -> std::vector<Condition> {
    std::vector<Condition> conds;
    conds.reserve(sv_conds.size());
    for (const auto &expr : sv_conds) {
        Condition cond;
        cond.lhs_col = {.tab_name = expr->lhs->tab_name, .col_name = expr->lhs->col_name};
        switch (expr->op) {
            case ast::SV_OP_EQ: cond.op = OP_EQ; break;
            case ast::SV_OP_NE: cond.op = OP_NE; break;
            case ast::SV_OP_LT: cond.op = OP_LT; break;
            case ast::SV_OP_GT: cond.op = OP_GT; break;
            case ast::SV_OP_LE: cond.op = OP_LE; break;
            case ast::SV_OP_GE: cond.op = OP_GE; break;
        }
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            if (auto int_val = std::dynamic_pointer_cast<ast::IntLit>(rhs_val)) {
                cond.rhs_val.set_int(int_val->val);
            } else if (auto float_val = std::dynamic_pointer_cast<ast::FloatLit>(rhs_val)) {
                cond.rhs_val.set_float(float_val->val);
            } else if (auto string_val = std::dynamic_pointer_cast<ast::StringLit>(rhs_val)) {
                cond.rhs_val.set_str(string_val->val);
            } else if (auto bool_val = std::dynamic_pointer_cast<ast::BoolLit>(rhs_val)) {
                cond.rhs_val.set_int(bool_val->val ? 1 : 0);
            } else {
                throw InternalError("Unexpected join literal type");
            }
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            cond.is_rhs_val = false;
            cond.rhs_col = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name};
        } else {
            throw InternalError("Unexpected join expression rhs");
        }
        conds.push_back(std::move(cond));
    }
    return conds;
}

}  // namespace

void prepare_index_lookup_values(const IndexMeta &index_meta, std::vector<Condition> &lookup_conds) {
    for (auto &cond : lookup_conds) {
        if (!cond.is_rhs_val || cond.rhs_val.raw) {
            continue;
        }
        auto col_it = std::find_if(index_meta.cols.begin(), index_meta.cols.end(),
                                   [&](const ColMeta &col) { return col.name == cond.lhs_col.col_name; });
        if (col_it != index_meta.cols.end()) {
            cond.rhs_val.init_raw(col_it->len);
        }
    }
}

std::vector<TabCol> Planner::collect_scan_required_cols(const Query &query, const std::string &tab_name) const {
    std::vector<TabCol> required_cols;
    auto append_col = [&](const TabCol &col) {
        if (col.tab_name != tab_name) {
            return;
        }
        if (!contains_col(required_cols, col)) {
            required_cols.push_back(col);
        }
    };

    for (const auto &cond : query.conds) {
        append_col(cond.lhs_col);
        if (!cond.is_rhs_val) {
            append_col(cond.rhs_col);
        }
    }
    if (auto select = std::dynamic_pointer_cast<ast::SelectStmt>(query.parse)) {
        for (const auto &join_expr : select->jointree) {
            auto join_conds = convert_join_expr_conds(join_expr->conds);
            for (const auto &cond : join_conds) {
                append_col(cond.lhs_col);
                if (!cond.is_rhs_val) {
                    append_col(cond.rhs_col);
                }
            }
        }
    }
    for (const auto &col : query.cols) {
        append_col(col);
    }
    for (const auto &agg : query.agg_infos) {
        if (!agg.is_star) {
            append_col(agg.col);
        }
    }
    for (const auto &col : query.group_by_cols) {
        append_col(col);
    }
    for (const auto &having : query.having_conds) {
        if (having.is_agg) {
            if (!having.agg.is_star) {
                append_col(having.agg.col);
            }
        } else {
            append_col(having.col);
        }
    }
    return required_cols;
}

std::vector<TabCol> Planner::collect_dml_required_cols(const Query &query, const std::string &tab_name) const {
    std::vector<TabCol> required_cols;
    auto append_col = [&](const TabCol &col) {
        if (col.tab_name != tab_name) {
            return;
        }
        if (!contains_col(required_cols, col)) {
            required_cols.push_back(col);
        }
    };

    for (const auto &cond : query.conds) {
        append_col(cond.lhs_col);
        if (!cond.is_rhs_val) {
            append_col(cond.rhs_col);
        }
    }
    for (const auto &set_clause : query.set_clauses) {
        append_col(set_clause.lhs);
    }
    return required_cols;
}

Planner::ScanBuildResult Planner::make_scan_plan(const std::string &tab_name,
                                                 const std::vector<Condition> &semantic_conds,
                                                 std::vector<TabCol> required_cols,
                                                 std::string visible_name) {
    ScanBuildResult result;
    auto display_name = visible_name.empty() ? tab_name : std::move(visible_name);
    auto table_cols = sm_manager_->db_.get_table(tab_name).cols;
    for (auto &col : table_cols) {
        col.tab_name = display_name;
    }
    auto normalized = normalize_predicates(table_cols, semantic_conds);
    if (normalized.contradiction) {
        result.scan = std::make_shared<ScanPlan>(sm_manager_, tab_name, std::vector<Condition>(), true,
                                                 display_name);
        return result;
    }
    const auto &tab = sm_manager_->db_.get_table(tab_name);
    auto best_match = match_best_index(tab, normalized.normalized_conds, required_cols);
    if (!best_match.matched) {
        result.scan = std::make_shared<ScanPlan>(sm_manager_, tab_name, std::vector<Condition>(), false,
                                                 display_name);
        result.filter_conds = std::move(normalized.normalized_conds);
        return result;
    }
    if (best_match.index_meta.has_value()) {
        prepare_index_lookup_values(*best_match.index_meta, best_match.lookup_conds);
    }
    result.scan = std::make_shared<ScanPlan>(sm_manager_, tab_name,
                                             std::move(best_match.lookup_conds),
                                             std::move(best_match.residual_conds),
                                             std::move(best_match.index_col_names),
                                             std::move(best_match.index_meta), display_name);
    return result;
}

/**
 * @brief 表算子条件谓词生成
 *
 * @param conds 条件
 * @param tab_names 表名
 * @return std::vector<Condition>
 */
std::vector<Condition> pop_conds(std::vector<Condition> &conds, std::string tab_names) {
    // auto has_tab = [&](const std::string &tab_name) {
    //     return std::find(tab_names.begin(), tab_names.end(), tab_name) != tab_names.end();
    // };
    std::vector<Condition> solved_conds;
    auto it = conds.begin();
    while (it != conds.end()) {
        if ((tab_names.compare(it->lhs_col.tab_name) == 0 && it->is_rhs_val) || (it->lhs_col.tab_name.compare(it->rhs_col.tab_name) == 0 && it->lhs_col.tab_name.compare(tab_names) == 0)) {
            solved_conds.emplace_back(std::move(*it));
            it = conds.erase(it);
        } else {
            it++;
        }
    }
    return solved_conds;
}

int push_conds(Condition *cond, std::shared_ptr<Plan> plan)
{
    if(auto x = std::dynamic_pointer_cast<ScanPlan>(plan))
    {
        if(x->tab_name_.compare(cond->lhs_col.tab_name) == 0) {
            return 1;
        } else if(x->tab_name_.compare(cond->rhs_col.tab_name) == 0){
            return 2;
        } else {
            return 0;
        }
    }
    else if(auto x = std::dynamic_pointer_cast<JoinPlan>(plan))
    {
        int left_res = push_conds(cond, x->left_);
        // 条件已经下推到左子节点
        if(left_res == 3){
            return 3;
        }
        int right_res = push_conds(cond, x->right_);
        // 条件已经下推到右子节点
        if(right_res == 3){
            return 3;
        }
        // 左子节点或右子节点有一个没有匹配到条件的列
        if(left_res == 0 || right_res == 0) {
            return left_res + right_res;
        }
        // 左子节点匹配到条件的右边
        if(left_res == 2) {
            // 需要将左右两边的条件变换位置
            std::swap(cond->lhs_col, cond->rhs_col);
            cond->op = kSwapOp.at(cond->op);
        }
        x->conds_.emplace_back(std::move(*cond));
        return 3;
    }
    return false;
}

std::shared_ptr<Plan> pop_scan(int *scantbl, std::string table, std::vector<std::string> &joined_tables, 
                std::vector<std::shared_ptr<Plan>> plans)
{
    for (size_t i = 0; i < plans.size(); i++) {
        auto x = std::dynamic_pointer_cast<ScanPlan>(plans[i]);
        if(x->tab_name_.compare(table) == 0)
        {
            scantbl[i] = 1;
            joined_tables.emplace_back(x->tab_name_);
            return plans[i];
        }
    }
    return nullptr;
}


std::shared_ptr<Plan> Planner::build_dml_scan_plan(const LogicalPlanContext &plan_context,
                                                   const std::string &tab_name) {
    std::vector<Condition> table_conds;
    auto it = plan_context.table_conds.find(tab_name);
    if (it != plan_context.table_conds.end()) {
        table_conds = it->second;
    }
    std::vector<TabCol> required_cols;
    auto cols_it = plan_context.table_required_cols.find(tab_name);
    if (cols_it != plan_context.table_required_cols.end()) {
        required_cols = cols_it->second;
    }
    auto scan_result = make_scan_plan(tab_name, table_conds, std::move(required_cols));
    if (plan_context.empty_tables.count(tab_name) != 0) {
        scan_result.scan->empty_result_ = true;
    }
    if (scan_result.filter_conds.empty()) {
        return scan_result.scan;
    }
    return std::make_shared<FilterPlan>(std::move(scan_result.scan), std::move(scan_result.filter_conds));
}

Planner::LogicalPlanContext Planner::logical_optimization(const std::shared_ptr<Query> &query, Context *context)
{
    LogicalPlanContext plan_context;
    std::map<std::string, std::vector<Condition>> raw_table_conds;
    ColumnUsageCollector column_usage;

    auto classify_condition = [&](const Condition &cond, std::vector<Condition> *join_conds) {
        plan_context.all_conds.push_back(cond);
        if (cond.is_rhs_val) {
            raw_table_conds[cond.lhs_col.tab_name].push_back(cond);
        } else if (cond.lhs_col.tab_name == cond.rhs_col.tab_name) {
            raw_table_conds[cond.lhs_col.tab_name].push_back(cond);
        } else if (join_conds != nullptr) {
            join_conds->push_back(cond);
            column_usage.collect_condition(cond);
        }
    };

    for (const auto &cond : query->conds) {
        classify_condition(cond, &plan_context.join_conds);
    }

    if (auto select = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {
        for (const auto &join_expr : select->jointree) {
            std::vector<Condition> explicit_join_conds;
            auto join_conds = convert_join_expr_conds(join_expr->conds);
            for (const auto &cond : join_conds) {
                classify_condition(cond, &explicit_join_conds);
            }
            plan_context.explicit_joins.push_back(
                LogicalJoin{join_expr->left, join_expr->right, std::move(explicit_join_conds), join_expr->type});
        }
        if (select->has_sort) {
            TabCol order_col = {.tab_name = select->order->cols->tab_name, .col_name = select->order->cols->col_name};
            if (order_col.tab_name.empty()) {
                std::string resolved_table;
                bool ambiguous = false;
                for (const auto &tab_name : query->tables) {
                    auto storage_it = query->table_storage_names.find(tab_name);
                    const auto &storage_name = storage_it == query->table_storage_names.end() ? tab_name : storage_it->second;
                    const auto &cols = sm_manager_->db_.get_table(storage_name).cols;
                    auto it = std::find_if(cols.begin(), cols.end(),
                                           [&](const ColMeta &col) { return col.name == order_col.col_name; });
                    if (it == cols.end()) {
                        continue;
                    }
                    if (!resolved_table.empty()) {
                        ambiguous = true;
                        break;
                    }
                    resolved_table = tab_name;
                }
                if (!ambiguous) {
                    order_col.tab_name = std::move(resolved_table);
                }
            }
            column_usage.collect_col(order_col);
        }
    }

    for (auto &[tab_name, conds] : raw_table_conds) {
        auto storage_it = query->table_storage_names.find(tab_name);
        const auto &storage_name = storage_it == query->table_storage_names.end() ? tab_name : storage_it->second;
        auto table_cols = sm_manager_->db_.get_table(storage_name).cols;
        for (auto &col : table_cols) {
            col.tab_name = tab_name;
        }
        auto normalized = normalize_predicates(table_cols, conds);
        if (normalized.contradiction) {
            plan_context.table_conds[tab_name].clear();
            plan_context.empty_tables.insert(tab_name);
        } else {
            plan_context.table_conds[tab_name] = std::move(normalized.normalized_conds);
        }
    }

    column_usage.collect_cols(query->cols);
    column_usage.collect_aggregates(query->agg_infos);
    column_usage.collect_cols(query->group_by_cols);
    column_usage.collect_having(query->having_conds);
    column_usage.collect_assignments(query->set_clauses);

    for (const auto &tab_name : query->tables) {
        plan_context.table_required_cols[tab_name] = column_usage.columns_for_table(tab_name);
    }

    return plan_context;
}

std::shared_ptr<Plan> Planner::physical_optimization(std::shared_ptr<Query> query,
                                                     const LogicalPlanContext &plan_context,
                                                     Context *context)
{
    std::shared_ptr<Plan> plan = make_one_rel(query, plan_context);
    
    // 其他物理优化

    // 处理orderby
    plan = generate_sort_plan(query, std::move(plan)); 

    return plan;
}



std::vector<std::pair<std::string, std::shared_ptr<Plan>>> Planner::build_table_plans(
    const std::shared_ptr<Query> &query, const LogicalPlanContext &plan_context) {
    std::vector<std::pair<std::string, std::shared_ptr<Plan>>> result;
    result.reserve(query->tables.size());

    for (const auto &tab_name : query->tables) {
        auto required_cols = plan_context.table_required_cols.at(tab_name);
        std::vector<Condition> curr_conds;
        auto it = plan_context.table_conds.find(tab_name);
        if (it != plan_context.table_conds.end()) {
            curr_conds = it->second;
        }
        auto storage_it = query->table_storage_names.find(tab_name);
        const auto &storage_name = storage_it == query->table_storage_names.end() ? tab_name : storage_it->second;
        ScanBuildResult scan_result;
        scan_result.scan = std::make_shared<ScanPlan>(sm_manager_, storage_name, std::vector<Condition>(), false,
                                                      tab_name);
        scan_result.filter_conds = std::move(curr_conds);
        if (plan_context.empty_tables.count(tab_name) != 0) {
            scan_result.scan->empty_result_ = true;
        }
        std::shared_ptr<Plan> node = scan_result.scan;
        if (!scan_result.filter_conds.empty()) {
            node = std::make_shared<FilterPlan>(std::move(node), std::move(scan_result.filter_conds));
        }
        if (query->tables.size() > 1 && !query->display_wildcard && !required_cols.empty()) {
            node = std::make_shared<ProjectionPlan>(T_Projection, std::move(node), std::move(required_cols));
        }
        result.emplace_back(tab_name, std::move(node));
    }
    return result;
}

std::shared_ptr<Plan> Planner::build_join_tree(
    std::vector<std::pair<std::string, std::shared_ptr<Plan>>> &table_plans,
    std::vector<Condition> conds,
    const std::vector<LogicalJoin> &jointree) {
    auto subtrees = build_join_subtrees(table_plans);

    for (const auto &join_expr : jointree) {
        auto left_idx = find_subtree_index(subtrees, join_expr.left);
        auto right_idx = find_subtree_index(subtrees, join_expr.right);
        if (!left_idx.has_value() || !right_idx.has_value() || left_idx == right_idx) {
            throw InternalError("Explicit join inputs are not part of the query");
        }
        auto [left, right] = take_subtree_pair(subtrees, *left_idx, *right_idx);
        auto display_conds = collect_display_join_conditions(join_expr.conds, conds, left, right);
        auto join_conds = collect_join_conditions(join_expr.conds, conds, left, right);
        subtrees.push_back(make_join_subtree(sm_manager_, std::move(left), std::move(right),
                                             std::move(join_conds), join_expr.type, std::move(display_conds)));
    }

    while (join_from_pending_conditions(sm_manager_, subtrees, conds)) {
    }

    while (subtrees.size() > 1) {
        auto right = take_subtree(subtrees, 1);
        auto left = take_subtree(subtrees, 0);
        auto display_conds = collect_display_join_conditions(std::vector<Condition>(), conds, left, right);
        auto join_conds = collect_join_conditions(std::vector<Condition>(), conds, left, right);
        subtrees.push_back(make_join_subtree(sm_manager_, std::move(left), std::move(right),
                                             std::move(join_conds), INNER_JOIN, std::move(display_conds)));
    }

    if (!conds.empty()) {
        throw InternalError("Unable to bind cross-table predicates to join tree");
    }

    return subtrees.empty() ? nullptr : subtrees.front().plan;
}

std::shared_ptr<Plan> Planner::make_one_rel(std::shared_ptr<Query> query,
                                            const LogicalPlanContext &plan_context)
{
    auto table_plans = build_table_plans(query, plan_context);
    if (table_plans.size() == 1) {
        return table_plans[0].second;
    }
    return build_join_tree(table_plans, plan_context.join_conds, plan_context.explicit_joins);
}


std::shared_ptr<Plan> Planner::generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan)
{
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    if(!x->has_sort) {
        return plan;
    }
    std::vector<std::string> tables = query->tables;
    std::vector<ColMeta> all_cols;
    for (auto &sel_tab_name : tables) {
        // 这里db_不能写成get_db(), 注意要传指针
        auto storage_it = query->table_storage_names.find(sel_tab_name);
        const auto &storage_name = storage_it == query->table_storage_names.end() ? sel_tab_name : storage_it->second;
        const auto &sel_tab_cols = sm_manager_->db_.get_table(storage_name).cols;
        auto old_size = all_cols.size();
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
        for (auto it = all_cols.begin() + static_cast<std::ptrdiff_t>(old_size); it != all_cols.end(); ++it) {
            it->tab_name = sel_tab_name;
        }
    }
    TabCol sel_col;
    for (auto &col : all_cols) {
        if(col.name.compare(x->order->cols->col_name) == 0 )
        sel_col = {.tab_name = col.tab_name, .col_name = col.name};
    }
    return std::make_shared<SortPlan>(T_Sort, std::move(plan), sel_col, 
                                    x->order->orderby_dir == ast::OrderBy_DESC);
}


/**
 * @brief select plan 生成
 *
 * @param sel_cols select plan 选取的列
 * @param tab_names select plan 目标的表
 * @param conds select plan 选取条件
 */
std::shared_ptr<Plan> Planner::generate_select_plan(std::shared_ptr<Query> query, Context *context) {
    //逻辑优化
    auto plan_context = logical_optimization(query, context);

    //物理优化
    auto sel_cols = query->select_items.empty() ? query->cols : query->select_items;
    std::shared_ptr<Plan> plannerRoot = physical_optimization(query, plan_context, context);

    if (!query->agg_infos.empty() || !query->group_by_cols.empty() || !query->having_conds.empty()) {
        plannerRoot = std::make_shared<AggregationPlan>(std::move(plannerRoot), query->agg_infos,
                                                        query->group_by_cols, query->having_conds);
    }

    plannerRoot = std::make_shared<ProjectionPlan>(T_Projection, std::move(plannerRoot),
                                                        std::move(sel_cols));

    return plannerRoot;
}

// 生成DDL语句和DML语句的查询执行计划
std::shared_ptr<Plan> Planner::do_planner(std::shared_ptr<Query> query, Context *context)
{
    std::shared_ptr<Plan> plannerRoot;
    if (auto x = std::dynamic_pointer_cast<ast::CreateTable>(query->parse)) {
        // create table;
        std::vector<ColDef> col_defs;
        std::vector<IndexSpec> index_specs;
        for (auto &field : x->fields) {
            if (auto sv_col_def = std::dynamic_pointer_cast<ast::ColDef>(field)) {
                ColDef col_def = {.name = sv_col_def->col_name,
                                  .type = interp_sv_type(sv_col_def->type_len->type),
                                  .len = sv_col_def->type_len->len};
                col_defs.push_back(col_def);
                if (sv_col_def->unique) {
                    index_specs.push_back(IndexSpec{{sv_col_def->col_name}, true});
                }
            } else if (auto sv_unique_def = std::dynamic_pointer_cast<ast::UniqueDef>(field)) {
                index_specs.push_back(IndexSpec{sv_unique_def->col_names, true});
            } else {
                throw InternalError("Unexpected field type");
            }
        }
        plannerRoot = std::make_shared<DDLPlan>(T_CreateTable, x->tab_name, std::vector<std::string>(), col_defs, index_specs, false);
    } else if (auto x = std::dynamic_pointer_cast<ast::DropTable>(query->parse)) {
        // drop table;
        plannerRoot = std::make_shared<DDLPlan>(T_DropTable, x->tab_name, std::vector<std::string>(), std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::CreateIndex>(query->parse)) {
        // create index;
        plannerRoot = std::make_shared<DDLPlan>(T_CreateIndex, x->tab_name, x->col_names, std::vector<ColDef>(), std::vector<IndexSpec>(), x->unique);
    } else if (auto x = std::dynamic_pointer_cast<ast::DropIndex>(query->parse)) {
        // drop index
        plannerRoot = std::make_shared<DDLPlan>(T_DropIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(query->parse)) {
        // insert;
        plannerRoot = std::make_shared<DMLPlan>(T_Insert, std::shared_ptr<Plan>(),  x->tab_name,  
                                                    query->values, std::vector<Condition>(), std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(query->parse)) {
        // delete;
        // 生成表扫描方式
        std::shared_ptr<Plan> table_scan_executors;
        // 只有一张表，不需要进行物理优化了
        // int index_no = get_indexNo(x->tab_name, query->conds);
        auto plan_context = logical_optimization(query, context);
        table_scan_executors = build_dml_scan_plan(plan_context, x->tab_name);

        plannerRoot = std::make_shared<DMLPlan>(T_Delete, table_scan_executors, x->tab_name,
                                                std::vector<Value>(), std::vector<Condition>(), std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(query->parse)) {
        // update;
        // 生成表扫描方式
        auto plan_context = logical_optimization(query, context);
        std::shared_ptr<Plan> table_scan_executors = build_dml_scan_plan(plan_context, x->tab_name);
        plannerRoot = std::make_shared<DMLPlan>(T_Update, table_scan_executors, x->tab_name,
                                                     std::vector<Value>(), std::vector<Condition>(),
                                                     query->set_clauses);
    } else if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {

        if (query->is_explain_analyze) {
            plannerRoot = std::make_shared<ExplainPlan>(make_explain_lines(query, x, sm_manager_));
            return plannerRoot;
        }

        std::shared_ptr<plannerInfo> root = std::make_shared<plannerInfo>(x);
        bool is_explain_analyze = query->is_explain_analyze;
        auto table_display_names = query->table_display_names;
        bool display_wildcard = query->display_wildcard;
        // 生成select语句的查询执行计划
        std::shared_ptr<Plan> projection = generate_select_plan(std::move(query), context);
        plannerRoot = std::make_shared<DMLPlan>(T_select, projection, std::string(), std::vector<Value>(),
                                                    std::vector<Condition>(), std::vector<SetClause>(),
                                                    is_explain_analyze, std::move(table_display_names),
                                                    display_wildcard);
    } else {
        throw InternalError("Unexpected AST root");
    }
    return plannerRoot;
}
