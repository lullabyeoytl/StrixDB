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

JoinSubtree make_join_subtree(JoinSubtree left, JoinSubtree right,
                              std::vector<Condition> conds, JoinType join_type) {
    auto tables = std::move(left.tables);
    tables.insert(right.tables.begin(), right.tables.end());
    auto plan = std::make_shared<JoinPlan>(T_NestLoop, std::move(left.plan), std::move(right.plan),
                                           std::move(conds), join_type);
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
    return std::pair<size_t, size_t>{*lhs_idx, *rhs_idx};
}

bool join_from_pending_conditions(std::vector<JoinSubtree> &subtrees, std::vector<Condition> &pending_conds) {
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
    auto join_conds = collect_join_conditions(std::vector<Condition>(), pending_conds, left, right);
    subtrees.push_back(make_join_subtree(std::move(left), std::move(right), std::move(join_conds), INNER_JOIN));
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
        const auto &table_cols = sm_manager_->db_.get_table(storage_name).cols;
        if (query->tables.size() > 1 && !required_cols.empty() && required_cols.size() < table_cols.size()) {
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
        auto join_conds = collect_join_conditions(join_expr.conds, conds, left, right);
        subtrees.push_back(make_join_subtree(std::move(left), std::move(right),
                                             std::move(join_conds), join_expr.type));
    }

    while (join_from_pending_conditions(subtrees, conds)) {
    }

    while (subtrees.size() > 1) {
        auto right = take_subtree(subtrees, 1);
        auto left = take_subtree(subtrees, 0);
        auto join_conds = collect_join_conditions(std::vector<Condition>(), conds, left, right);
        subtrees.push_back(make_join_subtree(std::move(left), std::move(right),
                                             std::move(join_conds), INNER_JOIN));
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

        std::shared_ptr<plannerInfo> root = std::make_shared<plannerInfo>(x);
        bool is_explain_analyze = query->is_explain_analyze;
        auto table_display_names = query->table_display_names;
        bool display_wildcard = x->cols.empty();
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
