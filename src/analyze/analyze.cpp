/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "analyze.h"
#include "errors.h"

#include <algorithm>

namespace {

AggInfo convert_agg_func(const std::shared_ptr<ast::AggFunc> &sv_agg) {
    AggInfo agg;
    agg.agg_type = sv_agg->agg_type;
    agg.is_star = sv_agg->is_star;
    if (!sv_agg->is_star) {
        agg.col = {.tab_name = sv_agg->col->tab_name, .col_name = sv_agg->col->col_name};
    }
    return agg;
}

void append_unique_agg(std::vector<AggInfo> &agg_infos, const AggInfo &agg) {
    auto it = std::find_if(agg_infos.begin(), agg_infos.end(), [&](const AggInfo &existing) {
        return existing.equals(agg);
    });
    if (it == agg_infos.end()) {
        agg_infos.push_back(agg);
    }
}

}  // namespace

/**
 * @description: 分析器，进行语义分析和查询重写，需要检查不符合语义规定的部分
 * @param {shared_ptr<ast::TreeNode>} parse parser生成的结果集
 * @return {shared_ptr<Query>} Query 
 */
std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse)
{
    std::shared_ptr<Query> query = std::make_shared<Query>();
    if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse))
    {
        // 处理表名
        query->tables = std::move(x->tabs);
        /** TODO: 检查表是否存在 */
        for (auto &tab_name : query->tables) {
            if (!sm_manager_->db_.is_table(tab_name)) {
                throw TableNotFoundError(tab_name);
            }
        }
        for (auto &sv_sel_expr : x->cols) {
            if (auto sv_sel_col = std::dynamic_pointer_cast<ast::Col>(sv_sel_expr)) {
                TabCol sel_col = {.tab_name = sv_sel_col->tab_name, .col_name = sv_sel_col->col_name};
                query->cols.push_back(sel_col);
            } else if (auto sv_agg = std::dynamic_pointer_cast<ast::AggFunc>(sv_sel_expr)) {
                auto agg = convert_agg_func(sv_agg);
                query->agg_infos.push_back(agg);
            } else {
                throw InternalError("Unexpected select expression type");
            }
        }
        
        std::vector<ColMeta> all_cols;
        get_all_cols(query->tables, all_cols);
        std::vector<std::string> visible_tables = query->tables;
        bool all_semi_joins = !x->jointree.empty() &&
                              std::all_of(x->jointree.begin(), x->jointree.end(),
                                          [](const std::shared_ptr<ast::JoinExpr> &join_expr) {
                                              return join_expr->type == SEMI_JOIN;
                                          });
        if (all_semi_joins) {
            visible_tables = {x->jointree.front()->left};
        }
        std::vector<ColMeta> visible_cols;
        get_all_cols(visible_tables, visible_cols);
        if (x->cols.empty()) {
            // select all columns
            for (auto &col : visible_cols) {
                TabCol sel_col = {.tab_name = col.tab_name, .col_name = col.name};
                query->cols.push_back(sel_col);
                query->select_items.push_back(sel_col);
            }
        } else {
            // infer table name from column name
            for (auto &sel_col : query->cols) {
                check_column(visible_cols, sel_col);  // 列元数据校验
            }
            query->select_items.clear();
            size_t col_idx = 0;
            for (auto &sv_sel_expr : x->cols) {
                if (std::dynamic_pointer_cast<ast::Col>(sv_sel_expr)) {
                    query->select_items.push_back(query->cols[col_idx++]);
                } else if (auto sv_agg = std::dynamic_pointer_cast<ast::AggFunc>(sv_sel_expr)) {
                    query->select_items.push_back({std::string(), agg_output_name(convert_agg_func(sv_agg))});
                } else {
                    throw InternalError("Unexpected select expression type");
                }
            }
        }
        for (auto &agg : query->agg_infos) {
            if (!agg.is_star) {
                check_column(visible_cols, agg.col);
            }
        }
        if (x->has_group_by) {
            for (auto &sv_group_col : x->group_by->cols) {
                TabCol group_col = {.tab_name = sv_group_col->tab_name, .col_name = sv_group_col->col_name};
                check_column(visible_cols, group_col);
                query->group_by_cols.push_back(group_col);
            }
        }
        for (auto &sv_having : x->having_conds) {
            HavingCond having_cond;
            having_cond.is_agg = sv_having->is_agg;
            having_cond.op = convert_sv_comp_op(sv_having->op);
            if (sv_having->is_agg) {
                having_cond.agg = convert_agg_func(sv_having->agg);
                if (!having_cond.agg.is_star) {
                    check_column(visible_cols, having_cond.agg.col);
                }
                append_unique_agg(query->agg_infos, having_cond.agg);
            } else {
                having_cond.col = {.tab_name = sv_having->col->tab_name, .col_name = sv_having->col->col_name};
                check_column(visible_cols, having_cond.col);
            }
            auto rhs_val = std::dynamic_pointer_cast<ast::Value>(sv_having->rhs);
            if (rhs_val == nullptr) {
                throw InternalError("Unexpected HAVING rhs expression type");
            }
            having_cond.rhs_val = convert_sv_value(rhs_val);
            query->having_conds.push_back(having_cond);
        }
        check_aggregate(visible_cols, *query);
        if (!x->jointree.empty()) {
            for (auto &sv_join : x->jointree) {
                normalize_sv_conds(sv_join->conds, all_cols);
            }
        }
        //处理where条件
        get_clause(x->conds, query->conds);
        check_clause(all_cols, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        // 检查表是否存在
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        // 处理 SET 子句
        for (auto &sv_set : x->set_clauses) {
            SetClause set_clause;
            set_clause.lhs = {.tab_name = std::string(), .col_name = sv_set->col_name};
            set_clause.rhs = convert_sv_value(sv_set->val);
            query->set_clauses.push_back(set_clause);
        }
        // 校验 SET 中的列存在性并推断表名
        std::vector<ColMeta> upd_cols;
        get_all_cols({x->tab_name}, upd_cols);
        for (auto &set_clause : query->set_clauses) {
            check_column(upd_cols, set_clause.lhs);
        }
        // 处理 WHERE 条件
        get_clause(x->conds, query->conds);
        check_clause(upd_cols, query->conds);

    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
        //处理where条件
        get_clause(x->conds, query->conds);
        std::vector<ColMeta> del_cols;
        get_all_cols({x->tab_name}, del_cols);
        check_clause(del_cols, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
        // 处理insert 的values值
        for (auto &sv_val : x->vals) {
            query->values.push_back(convert_sv_value(sv_val));
        }
    } else {
        // do nothing
    }
    query->parse = std::move(parse);
    return query;
}


void Analyze::check_column(const std::vector<ColMeta> &all_cols, TabCol &target) {
    if (target.tab_name.empty()) {
        // Table name not specified, infer table name from column name
        std::string tab_name;
        for (auto &col : all_cols) {
            if (col.name == target.col_name) {
                if (!tab_name.empty()) {
                    throw AmbiguousColumnError(target.col_name);
                }
                tab_name = col.tab_name;
            }
        }
        if (tab_name.empty()) {
            throw ColumnNotFoundError(target.col_name);
        }
        target.tab_name = std::move(tab_name);
    } else {
        try {
            find_col_meta(all_cols, target);
        } catch (const RMDBError &e) {
            throw e;
        }
    }
}

void Analyze::get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols) {
    size_t total = 0;
    for (auto &name : tab_names) {
        total += sm_manager_->db_.get_table(name).cols.size();
    }
    all_cols.reserve(all_cols.size() + total);
    for (auto &sel_tab_name : tab_names) {
        // 这里db_不能写成get_db(), 注意要传指针
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds) {
    conds.clear();
    for (auto &expr : sv_conds) {
        Condition cond;
        cond.lhs_col = {.tab_name = expr->lhs->tab_name, .col_name = expr->lhs->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.rhs_val = convert_sv_value(rhs_val);
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            cond.is_rhs_val = false;
            cond.rhs_col = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name};
        }
        conds.push_back(cond);
    }
}

void Analyze::normalize_sv_conds(std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, const std::vector<ColMeta> &all_cols) {
    std::vector<Condition> conds;
    get_clause(sv_conds, conds);
    check_clause(all_cols, conds);
    for (size_t i = 0; i < sv_conds.size(); ++i) {
        sv_conds[i]->lhs->tab_name = conds[i].lhs_col.tab_name;
        if (!conds[i].is_rhs_val) {
            auto rhs_col = std::dynamic_pointer_cast<ast::Col>(sv_conds[i]->rhs);
            if (rhs_col != nullptr) {
                rhs_col->tab_name = conds[i].rhs_col.tab_name;
            }
        }
    }
}

void Analyze::check_clause(const std::vector<ColMeta> &all_cols, std::vector<Condition> &conds) {
    std::map<std::string, TabMeta> tab_cache;
    // Get raw values in where clause
    for (auto &cond : conds) {
        // Infer table name from column name
        check_column(all_cols, cond.lhs_col);
        if (!cond.is_rhs_val) {
            check_column(all_cols, cond.rhs_col);
        }
        auto tab_it = tab_cache.find(cond.lhs_col.tab_name);
        if (tab_it == tab_cache.end()) {
            tab_it = tab_cache.emplace(cond.lhs_col.tab_name, sm_manager_->db_.get_table(cond.lhs_col.tab_name)).first;
        }
        TabMeta &lhs_tab = tab_it->second;
        auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
        ColType lhs_type = lhs_col->type;
        ColType rhs_type;
        if (cond.is_rhs_val) {
            cond.rhs_val.init_raw(lhs_col->len);
            rhs_type = cond.rhs_val.type;
        } else {
            auto rhs_it = tab_cache.find(cond.rhs_col.tab_name);
            if (rhs_it == tab_cache.end()) {
                rhs_it = tab_cache.emplace(cond.rhs_col.tab_name, sm_manager_->db_.get_table(cond.rhs_col.tab_name)).first;
            }
            TabMeta &rhs_tab = rhs_it->second;
            auto rhs_col = rhs_tab.get_col(cond.rhs_col.col_name);
            rhs_type = rhs_col->type;
        }
        if (!are_comparable_types(lhs_type, rhs_type)) {
            throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
        }
    }
}


Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val) {
    Value val;
    auto *raw = sv_val.get();
    if (auto *int_lit = dynamic_cast<ast::IntLit *>(raw)) {
        val.set_int(int_lit->val);
    } else if (auto *float_lit = dynamic_cast<ast::FloatLit *>(raw)) {
        val.set_float(float_lit->val);
    } else if (auto *str_lit = dynamic_cast<ast::StringLit *>(raw)) {
        val.set_str(str_lit->val);
    } else if (auto *bool_lit = dynamic_cast<ast::BoolLit *>(raw)) {
        val.set_int(bool_lit->val ? 1 : 0);
    } else {
        throw InternalError("Unexpected sv value type");
    }
    return val;
}

CompOp Analyze::convert_sv_comp_op(ast::SvCompOp op) {
    switch (op) {
        case ast::SV_OP_EQ: return OP_EQ;
        case ast::SV_OP_NE: return OP_NE;
        case ast::SV_OP_LT: return OP_LT;
        case ast::SV_OP_GT: return OP_GT;
        case ast::SV_OP_LE: return OP_LE;
        case ast::SV_OP_GE: return OP_GE;
        default: throw InternalError("Unexpected comparison op");
    }
}

ColType Analyze::get_column_type(const std::vector<ColMeta> &all_cols, const TabCol &target) {
    auto col = find_col_meta(all_cols, target);
    return col.type;
}

ColType Analyze::agg_result_type(const AggInfo &agg, const std::vector<ColMeta> &all_cols) {
    if (agg.agg_type == AGG_COUNT) {
        return TYPE_INT;
    }

    ColType input_type = get_column_type(all_cols, agg.col);
    if ((agg.agg_type == AGG_SUM || agg.agg_type == AGG_AVG) && input_type == TYPE_STRING) {
        throw RMDBError("SUM/AVG only support INT or FLOAT columns");
    }
    if (agg.agg_type == AGG_AVG) {
        return TYPE_FLOAT;
    }
    return input_type;
}

void Analyze::check_aggregate(const std::vector<ColMeta> &all_cols, Query &query) {
    bool has_agg_func = !query.agg_infos.empty() ||
        std::any_of(query.having_conds.begin(), query.having_conds.end(), [](const HavingCond &cond) {
            return cond.is_agg;
        });
    bool has_aggregate = has_agg_func || !query.having_conds.empty();
    bool has_group_by = !query.group_by_cols.empty();
    if (!has_aggregate && !has_group_by) {
        return;
    }

    for (auto &agg : query.agg_infos) {
        (void)agg_result_type(agg, all_cols);
    }

    if (!has_group_by && has_agg_func && !query.cols.empty()) {
        throw RMDBError("Non-aggregated columns must appear in GROUP BY");
    }

    if (has_group_by) {
        for (auto &sel_col : query.cols) {
            if (!contains_col(query.group_by_cols, sel_col)) {
                throw RMDBError(sel_col.col_name + " must appear in GROUP BY");
            }
        }
    }

    for (auto &having_cond : query.having_conds) {
        ColType lhs_type;
        if (having_cond.is_agg) {
            lhs_type = agg_result_type(having_cond.agg, all_cols);
        } else {
            if (!contains_col(query.group_by_cols, having_cond.col)) {
                throw RMDBError(having_cond.col.col_name + " must appear in GROUP BY");
            }
            lhs_type = get_column_type(all_cols, having_cond.col);
        }

        if (!are_comparable_types(lhs_type, having_cond.rhs_val.type)) {
            throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(having_cond.rhs_val.type));
        }
    }
}
