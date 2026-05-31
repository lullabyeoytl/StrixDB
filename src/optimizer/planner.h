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

#include <cassert>
#include <cstring>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "execution/execution_defs.h"
#include "execution/execution_manager.h"
#include "record/rm.h"
#include "system/sm.h"
#include "common/context.h"
#include "plan.h"
#include "parser/parser.h"
#include "common/common.h"
#include "analyze/analyze.h"

class Planner {
   private:
    struct LogicalJoin {
        std::string left;
        std::string right;
        std::vector<Condition> conds;
        JoinType type;
    };

    struct LogicalPlanContext {
        std::vector<Condition> all_conds;
        std::map<std::string, std::vector<Condition>> table_conds;
        std::set<std::string> empty_tables;
        std::vector<Condition> join_conds;
        std::map<std::string, std::vector<TabCol>> table_required_cols;
        std::vector<LogicalJoin> explicit_joins;
    };

    struct ScanBuildResult {
        std::shared_ptr<ScanPlan> scan;
        std::vector<Condition> filter_conds;
    };

    SmManager *sm_manager_;

    bool enable_nestedloop_join = true;
    bool enable_sortmerge_join = false;

   public:
    Planner(SmManager *sm_manager) : sm_manager_(sm_manager) {}


    std::shared_ptr<Plan> do_planner(std::shared_ptr<Query> query, Context *context);
    std::vector<TabCol> collect_scan_required_cols(const Query &query, const std::string &tab_name) const;
    std::vector<TabCol> collect_dml_required_cols(const Query &query, const std::string &tab_name) const;

    void set_enable_nestedloop_join(bool set_val) { enable_nestedloop_join = set_val; }
    
    void set_enable_sortmerge_join(bool set_val) { enable_sortmerge_join = set_val; }
    
   private:
    LogicalPlanContext logical_optimization(const std::shared_ptr<Query> &query, Context *context);
    std::shared_ptr<Plan> physical_optimization(std::shared_ptr<Query> query,
                                                const LogicalPlanContext &plan_context,
                                                Context *context);

    std::shared_ptr<Plan> make_one_rel(std::shared_ptr<Query> query, const LogicalPlanContext &plan_context);

    std::vector<std::pair<std::string, std::shared_ptr<Plan>>> build_table_plans(
        const std::shared_ptr<Query> &query, const LogicalPlanContext &plan_context);

    std::shared_ptr<Plan> build_join_tree(
        std::vector<std::pair<std::string, std::shared_ptr<Plan>>> &table_plans,
        std::vector<Condition> conds,
        const std::vector<LogicalJoin> &jointree);

    std::shared_ptr<Plan> generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan);

std::shared_ptr<Plan> generate_select_plan(std::shared_ptr<Query> query, Context *context);


    // int get_indexNo(std::string tab_name, std::vector<Condition> curr_conds);
    ScanBuildResult make_scan_plan(const std::string &tab_name, const std::vector<Condition> &semantic_conds,
                                   std::vector<TabCol> required_cols = {});
    std::shared_ptr<Plan> build_dml_scan_plan(const LogicalPlanContext &plan_context, const std::string &tab_name);

    ColType interp_sv_type(ast::SvType sv_type) {
        std::map<ast::SvType, ColType> m = {
            {ast::SV_TYPE_INT, TYPE_INT}, {ast::SV_TYPE_FLOAT, TYPE_FLOAT}, {ast::SV_TYPE_STRING, TYPE_STRING}};
        return m.at(sv_type);
    }
};
