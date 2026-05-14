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

#include <memory>

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
auto convert_join_expr_conds(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds) -> std::vector<Condition>;

struct SortMergeLayout {
    bool supported = false;
    std::vector<Condition> merge_conds;
    std::vector<Condition> residual_conds;
    std::vector<TabCol> left_sort_cols;
    std::vector<TabCol> right_sort_cols;
};

auto sortmerge_supports_join_type(JoinType type) -> bool;
auto classify_sortmerge_layout(const std::vector<Condition> &conds, JoinType type) -> SortMergeLayout;
void finalize_join_plan_tree(const std::shared_ptr<Plan> &plan, bool prefer_sortmerge);
}

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

std::shared_ptr<ScanPlan> Planner::make_scan_plan(const std::string &tab_name, std::vector<Condition> conds,
                                                  std::vector<TabCol> required_cols) {
    const auto &table_cols = sm_manager_->db_.get_table(tab_name).cols;
    auto normalized = normalize_predicates(table_cols, conds);
    if (normalized.contradiction) {
        return std::make_shared<ScanPlan>(sm_manager_, tab_name, std::move(normalized.normalized_conds), true);
    }
    conds = std::move(normalized.normalized_conds);
    const auto &tab = sm_manager_->db_.get_table(tab_name);
    auto best_match = match_best_index(tab, conds, required_cols);
    if (!best_match.matched) {
        return std::make_shared<ScanPlan>(sm_manager_, tab_name, std::move(conds));
    }
    if (best_match.index_meta.has_value()) {
        prepare_index_lookup_values(*best_match.index_meta, best_match.lookup_conds);
    }
    return std::make_shared<ScanPlan>(sm_manager_, tab_name,
                                      std::move(best_match.lookup_conds),
                                      std::move(best_match.residual_conds),
                                      std::move(best_match.index_col_names),
                                      std::move(best_match.index_meta));
}

namespace {

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

auto pick_join_tag(bool enable_nestedloop_join, bool enable_sortmerge_join) -> PlanTag {
    if (enable_sortmerge_join) {
        return T_SortMerge;
    }
    if (enable_nestedloop_join) {
        return T_NestLoop;
    }
    throw RMDBError("No join executor selected!");
}

auto sortmerge_supports_join_type(JoinType type) -> bool {
    switch (type) {
        case INNER_JOIN:
        case SEMI_JOIN:
            return true;
        default:
            return false;
    }
}

auto classify_sortmerge_layout(const std::vector<Condition> &conds, JoinType type) -> SortMergeLayout {
    SortMergeLayout layout;
    layout.residual_conds = conds;
    if (!sortmerge_supports_join_type(type)) {
        return layout;
    }
    for (const auto &cond : conds) {
        if (cond.is_rhs_val || cond.op != OP_EQ) {
            continue;
        }
        layout.merge_conds.push_back(cond);
        layout.left_sort_cols.push_back(cond.lhs_col);
        layout.right_sort_cols.push_back(cond.rhs_col);
    }
    if (!layout.merge_conds.empty()) {
        layout.supported = true;
        layout.residual_conds.erase(
            std::remove_if(layout.residual_conds.begin(), layout.residual_conds.end(),
                           [](const Condition &cond) { return !cond.is_rhs_val && cond.op == OP_EQ; }),
            layout.residual_conds.end());
    }
    return layout;
}

void finalize_join_plan_tree(const std::shared_ptr<Plan> &plan, bool prefer_sortmerge) {
    auto join = std::dynamic_pointer_cast<JoinPlan>(plan);
    if (join == nullptr) {
        return;
    }
    finalize_join_plan_tree(join->left_, prefer_sortmerge);
    finalize_join_plan_tree(join->right_, prefer_sortmerge);

    join->merge_conds_.clear();
    join->residual_conds_ = join->conds_;
    join->left_sort_cols_.clear();
    join->right_sort_cols_.clear();

    if (!prefer_sortmerge) {
        join->tag = T_NestLoop;
        return;
    }

    auto layout = classify_sortmerge_layout(join->conds_, join->type);
    if (!layout.supported) {
        join->tag = T_NestLoop;
        return;
    }

    join->tag = T_SortMerge;
    join->merge_conds_ = std::move(layout.merge_conds);
    join->residual_conds_ = std::move(layout.residual_conds);
    join->left_sort_cols_ = std::move(layout.left_sort_cols);
    join->right_sort_cols_ = std::move(layout.right_sort_cols);
    // SortMergeJoin consumes sorted children. The planner injects explicit SortPlan
    // nodes so the portal only needs to dispatch executors by PlanTag.
    join->left_ = std::make_shared<SortPlan>(T_Sort, join->left_, join->left_sort_cols_,
                                             std::vector<bool>(join->left_sort_cols_.size(), false));
    join->right_ = std::make_shared<SortPlan>(T_Sort, join->right_, join->right_sort_cols_,
                                              std::vector<bool>(join->right_sort_cols_.size(), false));
}

}  // namespace

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


std::shared_ptr<Query> Planner::logical_optimization(std::shared_ptr<Query> query, Context *context)
{
    
    //TODO 实现逻辑优化规则

    return query;
}

std::shared_ptr<Plan> Planner::physical_optimization(std::shared_ptr<Query> query, Context *context)
{
    std::shared_ptr<Plan> plan = make_one_rel(query);
    
    // 其他物理优化

    // 处理orderby
    plan = generate_sort_plan(query, std::move(plan)); 

    return plan;
}



std::shared_ptr<Plan> Planner::make_one_rel(std::shared_ptr<Query> query)
{
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    std::vector<std::string> tables = query->tables;
    std::vector<Condition> pending_conds = query->conds;
    if (x != nullptr) {
        for (const auto &join_expr : x->jointree) {
            auto join_conds = convert_join_expr_conds(join_expr->conds);
            pending_conds.insert(pending_conds.end(), join_conds.begin(), join_conds.end());
        }
    }
    // // Scan table , 生成表算子列表tab_nodes
    std::vector<std::shared_ptr<Plan>> table_scan_executors(tables.size());
    for (size_t i = 0; i < tables.size(); i++) {
        auto curr_conds = pop_conds(pending_conds, tables[i]);
        auto required_cols = collect_scan_required_cols(*query, tables[i]);
        table_scan_executors[i] = make_scan_plan(tables[i], std::move(curr_conds), std::move(required_cols));
    }
    // 只有一个表，不需要join。
    if(tables.size() == 1)
    {
        return table_scan_executors[0];
    }
    // 获取where条件
    auto conds = std::move(pending_conds);
    std::shared_ptr<Plan> table_join_executors;
    PlanTag join_tag = pick_join_tag(enable_nestedloop_join, enable_sortmerge_join);
    
    int scantbl[tables.size()];
    for(size_t i = 0; i < tables.size(); i++)
    {
        scantbl[i] = -1;
    }

    std::vector<std::string> joined_tables;
    if (x != nullptr && !x->jointree.empty()) {
        for (const auto &join_expr : x->jointree) {
            bool left_joined = std::find(joined_tables.begin(), joined_tables.end(), join_expr->left) != joined_tables.end();
            bool right_joined = std::find(joined_tables.begin(), joined_tables.end(), join_expr->right) != joined_tables.end();
            std::shared_ptr<Plan> left = nullptr;
            std::shared_ptr<Plan> right = nullptr;

            if (!left_joined) {
                left = pop_scan(scantbl, join_expr->left, joined_tables, table_scan_executors);
            }
            if (!right_joined) {
                right = pop_scan(scantbl, join_expr->right, joined_tables, table_scan_executors);
            }

            if (table_join_executors == nullptr) {
                table_join_executors = std::make_shared<JoinPlan>(join_tag, std::move(left), std::move(right),
                                                                  std::vector<Condition>(), join_expr->type);
            } else if (left_joined && !right_joined) {
                table_join_executors = std::make_shared<JoinPlan>(join_tag, std::move(table_join_executors),
                                                                  std::move(right), std::vector<Condition>(),
                                                                  join_expr->type);
            } else if (!left_joined && right_joined) {
                table_join_executors = std::make_shared<JoinPlan>(join_tag, std::move(left),
                                                                  std::move(table_join_executors),
                                                                  std::vector<Condition>(), join_expr->type);
            } else if (!left_joined && !right_joined) {
                auto pair_join = std::make_shared<JoinPlan>(join_tag, std::move(left), std::move(right),
                                                            std::vector<Condition>(), join_expr->type);
                table_join_executors = std::make_shared<JoinPlan>(join_tag, std::move(pair_join),
                                                                  std::move(table_join_executors),
                                                                  std::vector<Condition>());
            }
        }
    }

    if(table_join_executors == nullptr && conds.size() >= 1)
    {
        // 有连接条件

        // 根据连接条件，生成第一层join
        auto it = conds.begin();
        while (it != conds.end()) {
            std::shared_ptr<Plan> left , right;
            left = pop_scan(scantbl, it->lhs_col.tab_name, joined_tables, table_scan_executors);
            right = pop_scan(scantbl, it->rhs_col.tab_name, joined_tables, table_scan_executors);
            std::vector<Condition> join_conds{*it};
            //建立join
            table_join_executors = std::make_shared<JoinPlan>(join_tag, std::move(left), std::move(right), join_conds);

            // table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right), join_conds);
            it = conds.erase(it);
            break;
        }
    } else if (table_join_executors == nullptr) {
        table_join_executors = table_scan_executors[0];
        scantbl[0] = 1;
        joined_tables.emplace_back(tables[0]);
    }

    if (table_join_executors != nullptr) {
        auto it = conds.begin();
        while (it != conds.end()) {
            std::shared_ptr<Plan> left_need_to_join_executors = nullptr;
            std::shared_ptr<Plan> right_need_to_join_executors = nullptr;
            bool isneedreverse = false;
            if (std::find(joined_tables.begin(), joined_tables.end(), it->lhs_col.tab_name) == joined_tables.end()) {
                left_need_to_join_executors = pop_scan(scantbl, it->lhs_col.tab_name, joined_tables, table_scan_executors);
            }
            if (!it->is_rhs_val &&
                std::find(joined_tables.begin(), joined_tables.end(), it->rhs_col.tab_name) == joined_tables.end()) {
                right_need_to_join_executors = pop_scan(scantbl, it->rhs_col.tab_name, joined_tables, table_scan_executors);
                isneedreverse = true;
            }

            if(left_need_to_join_executors != nullptr && right_need_to_join_executors != nullptr) {
                std::vector<Condition> join_conds{*it};
                std::shared_ptr<Plan> temp_join_executors = std::make_shared<JoinPlan>(join_tag,
                                                                    std::move(left_need_to_join_executors),
                                                                    std::move(right_need_to_join_executors),
                                                                    join_conds);
                table_join_executors = std::make_shared<JoinPlan>(join_tag, std::move(temp_join_executors),
                                                                    std::move(table_join_executors),
                                                                    std::vector<Condition>());
            } else if(left_need_to_join_executors != nullptr || right_need_to_join_executors != nullptr) {
                if(isneedreverse) {
                    std::swap(it->lhs_col, it->rhs_col);
                    it->op = kSwapOp.at(it->op);
                    left_need_to_join_executors = std::move(right_need_to_join_executors);
                }
                std::vector<Condition> join_conds{*it};
                table_join_executors = std::make_shared<JoinPlan>(join_tag, std::move(left_need_to_join_executors),
                                                                    std::move(table_join_executors), join_conds);
            } else if (!it->is_rhs_val) {
                push_conds(std::move(&(*it)), table_join_executors);
            }
            it = conds.erase(it);
        }
    }

    //连接剩余表
    for (size_t i = 0; i < tables.size(); i++) {
        if(scantbl[i] == -1) {
            table_join_executors = std::make_shared<JoinPlan>(join_tag, std::move(table_scan_executors[i]), 
                                                    std::move(table_join_executors), std::vector<Condition>());
        }
    }

    finalize_join_plan_tree(table_join_executors, enable_sortmerge_join);

    return table_join_executors;

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
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
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
    query = logical_optimization(std::move(query), context);

    //物理优化
    auto sel_cols = query->select_items.empty() ? query->cols : query->select_items;
    std::shared_ptr<Plan> plannerRoot = physical_optimization(query, context);

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
        table_scan_executors = make_scan_plan(x->tab_name, query->conds,
                                              collect_dml_required_cols(*query, x->tab_name));

        plannerRoot = std::make_shared<DMLPlan>(T_Delete, table_scan_executors, x->tab_name,  
                                                std::vector<Value>(), query->conds, std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(query->parse)) {
        // update;
        // 生成表扫描方式
        std::shared_ptr<Plan> table_scan_executors = make_scan_plan(
            x->tab_name, query->conds, collect_dml_required_cols(*query, x->tab_name));
        plannerRoot = std::make_shared<DMLPlan>(T_Update, table_scan_executors, x->tab_name,
                                                     std::vector<Value>(), query->conds, 
                                                     query->set_clauses);
    } else if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {

        std::shared_ptr<plannerInfo> root = std::make_shared<plannerInfo>(x);
        // 生成select语句的查询执行计划
        std::shared_ptr<Plan> projection = generate_select_plan(std::move(query), context);
        plannerRoot = std::make_shared<DMLPlan>(T_select, projection, std::string(), std::vector<Value>(),
                                                    std::vector<Condition>(), std::vector<SetClause>());
    } else {
        throw InternalError("Unexpected AST root");
    }
    return plannerRoot;
}
