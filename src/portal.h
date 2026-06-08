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
#include <cerrno>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include "optimizer/plan.h"
#include "execution/executor_abstract.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_filter.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_update.h"
#include "execution/executor_insert.h"
#include "execution/executor_delete.h"
#include "execution/executor_aggregation.h"
#include "execution/execution_sort.h"
#include "common/common.h"

namespace explain_analyze_detail {
constexpr int kTreeMaxDepth = 64;
}

typedef enum portalTag{
    PORTAL_Invalid_Query = 0,
    PORTAL_ONE_SELECT,
    PORTAL_EXPLAIN_ANALYZE,
    PORTAL_DML_WITHOUT_SELECT,
    PORTAL_MULTI_QUERY,
    PORTAL_CMD_UTILITY
} portalTag;


struct PortalStmt {
    portalTag tag;
    
    std::vector<TabCol> sel_cols;
    std::unique_ptr<AbstractExecutor> root;
    std::shared_ptr<Plan> plan;
    
    PortalStmt(portalTag tag_, std::vector<TabCol> sel_cols_, std::unique_ptr<AbstractExecutor> root_, std::shared_ptr<Plan> plan_) :
            tag(tag_), sel_cols(std::move(sel_cols_)), root(std::move(root_)), plan(std::move(plan_)) {}
};

/**
 * @brief Portal模块负责将查询执行计划转换成对应的算子树，并执行算子树生成结果
 */
class Portal
{
   private:
    SmManager *sm_manager_;

    std::map<std::string, std::string> table_display_names_;

    std::string format_col(const TabCol &col) const {
        if (col.tab_name.empty()) {
            return col.col_name;
        }
        std::string table = col.tab_name;
        auto it = table_display_names_.find(col.tab_name);
        if (it != table_display_names_.end()) {
            table = it->second;
        }
        return table + "." + col.col_name;
    }

    std::string format_col_list(const std::vector<TabCol> &cols) const {
        if (cols.empty()) {
            return "[*]";
        }
        std::vector<std::string> rendered;
        rendered.reserve(cols.size());
        for (const auto &col : cols) {
            rendered.push_back(format_col(col));
        }
        std::sort(rendered.begin(), rendered.end());
        std::string result = "[";
        for (size_t i = 0; i < rendered.size(); ++i) {
            if (i != 0) {
                result += ", ";
            }
            result += rendered[i];
        }
        result += "]";
        return result;
    }

    std::string format_value(const Value &value) const {
        std::ostringstream os;
        if (value.type == TYPE_INT) {
            return std::to_string(value.int_val);
        }
        if (value.type == TYPE_FLOAT) {
            os << value.float_val;
            return os.str();
        }
        if (value.type == TYPE_STRING) {
            return value.str_val;
        }
        throw InternalError("Unexpected value type in explain output");
    }

    std::string format_op(CompOp op) const {
        switch (op) {
            case OP_EQ: return "=";
            case OP_NE: return "<>";
            case OP_LT: return "<";
            case OP_GT: return ">";
            case OP_LE: return "<=";
            case OP_GE: return ">=";
        }
        throw InternalError("Unexpected comparison op");
    }

    std::string format_condition(const Condition &cond) const {
        std::string rhs = cond.is_rhs_val ? format_value(cond.rhs_val) : format_col(cond.rhs_col);
        return format_col(cond.lhs_col) + format_op(cond.op) + rhs;
    }

    std::string format_condition_list(const std::vector<Condition> &conds) const {
        std::vector<std::string> rendered;
        rendered.reserve(conds.size());
        for (const auto &cond : conds) {
            rendered.push_back(format_condition(cond));
        }
        std::sort(rendered.begin(), rendered.end());
        std::string result = "[";
        for (size_t i = 0; i < rendered.size(); ++i) {
            if (i != 0) {
                result += ", ";
            }
            result += rendered[i];
        }
        result += "]";
        return result;
    }

    void collect_plan_tables(const std::shared_ptr<Plan> &plan, std::set<std::string> &tables,
                             int depth = 0) const {
        if (plan == nullptr) {
            return;
        }
        if (depth >= explain_analyze_detail::kTreeMaxDepth) {
            throw InternalError("Plan tree depth exceeds limit");
        }
        if (auto scan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
            tables.insert(scan->tab_name_);
            return;
        }
        if (auto project = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
            collect_plan_tables(project->subplan_, tables, depth + 1);
            return;
        }
        if (auto filter = std::dynamic_pointer_cast<FilterPlan>(plan)) {
            collect_plan_tables(filter->subplan_, tables, depth + 1);
            return;
        }
        if (auto aggregation = std::dynamic_pointer_cast<AggregationPlan>(plan)) {
            collect_plan_tables(aggregation->subplan_, tables, depth + 1);
            return;
        }
        if (auto sort = std::dynamic_pointer_cast<SortPlan>(plan)) {
            collect_plan_tables(sort->subplan_, tables, depth + 1);
            return;
        }
        if (auto join = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            collect_plan_tables(join->left_, tables, depth + 1);
            collect_plan_tables(join->right_, tables, depth + 1);
            return;
        }
    }

    std::string format_table_list(const std::shared_ptr<Plan> &plan) const {
        std::set<std::string> tables;
        collect_plan_tables(plan, tables);
        std::string result = "[";
        size_t i = 0;
        for (const auto &table : tables) {
            if (i != 0) {
                result += ", ";
            }
            result += table;
            ++i;
        }
        result += "]";
        return result;
    }

   public:
    Portal(SmManager *sm_manager) : sm_manager_(sm_manager){}
    ~Portal(){}

    // 将查询执行计划转换成对应的算子树
    std::shared_ptr<PortalStmt> start(std::shared_ptr<Plan> plan, Context *context)
    {
        // 这里可以将select进行拆分，例如：一个select，带有return的select等
        if (auto x = std::dynamic_pointer_cast<OtherPlan>(plan)) {
            return std::make_shared<PortalStmt>(PORTAL_CMD_UTILITY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(),plan);
        } else if(auto x = std::dynamic_pointer_cast<SetKnobPlan>(plan)) {
            return std::make_shared<PortalStmt>(PORTAL_CMD_UTILITY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(), plan); 
        } else if (auto x = std::dynamic_pointer_cast<DDLPlan>(plan)) {
            return std::make_shared<PortalStmt>(PORTAL_MULTI_QUERY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(),plan);
        } else if (auto x = std::dynamic_pointer_cast<DMLPlan>(plan)) {
            table_display_names_ = x->table_display_names_;
            switch(x->tag) {
                case T_select:
                {
                    std::shared_ptr<ProjectionPlan> p = std::dynamic_pointer_cast<ProjectionPlan>(x->subplan_);
                    std::unique_ptr<AbstractExecutor> root= convert_plan_executor(p, context);
                    if (x->is_explain_analyze_ && x->display_wildcard_) {
                        root->set_explain_info("Project", "columns=[*]");
                    }
                    auto tag = x->is_explain_analyze_ ? PORTAL_EXPLAIN_ANALYZE : PORTAL_ONE_SELECT;
                    return std::make_shared<PortalStmt>(tag, std::move(p->sel_cols_), std::move(root), plan);
                }
                    
                case T_Update:
                {
                    std::unique_ptr<AbstractExecutor> scan= convert_plan_executor(x->subplan_, context);
                    std::vector<Rid> rids;
                    for (scan->beginTuple(); !scan->is_end(); scan->nextTuple()) {
                        rids.push_back(scan->rid());
                    }
                    std::unique_ptr<AbstractExecutor> root =std::make_unique<UpdateExecutor>(sm_manager_, 
                                                            x->tab_name_, x->set_clauses_, x->conds_, rids, context);
                    return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
                }
                case T_Delete:
                {
                    std::unique_ptr<AbstractExecutor> scan= convert_plan_executor(x->subplan_, context);
                    std::vector<Rid> rids;
                    for (scan->beginTuple(); !scan->is_end(); scan->nextTuple()) {
                        rids.push_back(scan->rid());
                    }

                    std::unique_ptr<AbstractExecutor> root =
                        std::make_unique<DeleteExecutor>(sm_manager_, x->tab_name_, x->conds_, rids, context);

                    return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
                }

                case T_Insert:
                {
                    std::unique_ptr<AbstractExecutor> root =
                            std::make_unique<InsertExecutor>(sm_manager_, x->tab_name_, x->values_, context);
            
                    return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
                }


                default:
                    throw InternalError("Unexpected field type");
                    break;
            }
        } else {
            throw InternalError("Unexpected field type");
        }
        return nullptr;
    }

    // 遍历算子树并执行算子生成执行结果
    void run(std::shared_ptr<PortalStmt> portal, QlManager* ql, txn_id_t *txn_id, Context *context){
        switch(portal->tag) {
            case PORTAL_ONE_SELECT:
            {
                ql->select_from(std::move(portal->root), std::move(portal->sel_cols), context);
                break;
            }

            case PORTAL_EXPLAIN_ANALYZE:
            {
                ql->explain_analyze(std::move(portal->root), context);
                break;
            }

            case PORTAL_DML_WITHOUT_SELECT:
            {
                ql->run_dml(std::move(portal->root));
                break;
            }
            case PORTAL_MULTI_QUERY:
            {
                ql->run_mutli_query(portal->plan, context);
                break;
            }
            case PORTAL_CMD_UTILITY:
            {
                ql->run_cmd_utility(portal->plan, txn_id, context);
                break;
            }
            default:
            {
                throw InternalError("Unexpected field type");
            }
        }
    }

    // 清空资源
    void drop(){}


    std::unique_ptr<AbstractExecutor> convert_plan_executor(std::shared_ptr<Plan> plan, Context *context)
    {
        if(auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)){
            auto executor = std::make_unique<ProjectionExecutor>(convert_plan_executor(x->subplan_, context),
                                                                 x->sel_cols_);
            executor->set_explain_info("Project", "columns=" + format_col_list(x->sel_cols_));
            return executor;
        } else if(auto x = std::dynamic_pointer_cast<FilterPlan>(plan)) {
            auto executor = std::make_unique<FilterExecutor>(convert_plan_executor(x->subplan_, context), x->conds_);
            executor->set_explain_info("Filter", "condition=" + format_condition_list(x->conds_));
            return executor;
        } else if(auto x = std::dynamic_pointer_cast<AggregationPlan>(plan)) {
            auto executor = std::make_unique<AggregationExecutor>(convert_plan_executor(x->subplan_, context),
                                                                  x->agg_infos_, x->group_by_cols_,
                                                                  x->having_conds_);
            executor->set_explain_info("Aggregation");
            return executor;
        } else if(auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
            if(x->tag == T_SeqScan) {
                auto executor = std::make_unique<SeqScanExecutor>(sm_manager_, x->tab_name_, x->all_conds_,
                                                                  x->empty_result_, context);
                executor->set_explain_info("Scan", "table=" + x->tab_name_ + ", type=SeqScan");
                return executor;
            }
            else {
                auto executor = std::make_unique<IndexScanExecutor>(sm_manager_, x->tab_name_, x->index_lookup_conds_,
                                                                    x->residual_conds_, x->index_col_names_,
                                                                    x->index_meta_, context);
                executor->set_explain_info("Scan", "table=" + x->tab_name_ + ", type=IndexScan");
                return executor;
            }
        } else if(auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            std::unique_ptr<AbstractExecutor> left = convert_plan_executor(x->left_, context);
            std::unique_ptr<AbstractExecutor> right = convert_plan_executor(x->right_, context);
            auto executor = std::make_unique<NestedLoopJoinExecutor>(
                                std::move(left),
                                std::move(right), x->conds_, x->type, x->reverse_right_scan_);
            executor->set_explain_info("Join", "tables=" + format_table_list(plan) +
                                               ", condition=" + format_condition_list(x->conds_));
            return executor;
        } else if(auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
            auto executor = std::make_unique<SortExecutor>(convert_plan_executor(x->subplan_, context),
                                                           x->sel_col_, x->is_desc_);
            executor->set_explain_info("Sort");
            return executor;
        }
        return nullptr;
    }

};
