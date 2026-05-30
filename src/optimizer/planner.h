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
    SmManager *sm_manager_;

    bool enable_nestedloop_join = true;
    bool enable_sortmerge_join = true;
    bool enable_hash_join = true;

   public:
    Planner(SmManager *sm_manager) : sm_manager_(sm_manager) {}


    std::shared_ptr<Plan> do_planner(std::shared_ptr<Query> query, Context *context);
    std::vector<TabCol> collect_scan_required_cols(const Query &query, const std::string &exposed_name) const;
    std::vector<TabCol> collect_dml_required_cols(const Query &query, const std::string &tab_name) const;

    void set_enable_nestedloop_join(bool set_val) { enable_nestedloop_join = set_val; }

    void set_enable_sortmerge_join(bool set_val) { enable_sortmerge_join = set_val; }

    void set_enable_hash_join(bool set_val) { enable_hash_join = set_val; }

   private:
    std::shared_ptr<Query> logical_optimization(std::shared_ptr<Query> query, Context *context);
    std::shared_ptr<Plan> physical_optimization(std::shared_ptr<Query> query, Context *context);

    std::shared_ptr<Plan> make_one_rel(std::shared_ptr<Query> query);
    std::vector<Condition> collect_select_conds(const std::shared_ptr<Query> &query) const;
    std::vector<std::shared_ptr<Plan>> build_base_scan_plans(const Query &query,
                                                             std::vector<Condition> &pending_conds);
    std::shared_ptr<Plan> build_join_tree_from_jointree(
        const std::shared_ptr<ast::SelectStmt> &select,
        std::vector<std::shared_ptr<Plan>> &table_scan_executors,
        std::vector<int> &scantbl,
        std::vector<std::string> &joined_tables);
    std::shared_ptr<Plan> seed_join_tree_from_remaining_conds(
        std::vector<Condition> &conds,
        std::vector<std::shared_ptr<Plan>> &table_scan_executors,
        std::vector<int> &scantbl,
        std::vector<std::string> &joined_tables);
    void attach_remaining_conds(std::shared_ptr<Plan> &table_join_executors,
                                std::vector<Condition> &conds,
                                std::vector<std::shared_ptr<Plan>> &table_scan_executors,
                                std::vector<int> &scantbl,
                                std::vector<std::string> &joined_tables);
    void append_unjoined_scans(std::shared_ptr<Plan> &table_join_executors,
                               std::vector<std::shared_ptr<Plan>> &table_scan_executors,
                               std::vector<int> &scantbl);

    std::shared_ptr<Plan> generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan);

    std::shared_ptr<Plan> generate_limit_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan);

    std::shared_ptr<Plan> generate_aggregate_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan);

    std::shared_ptr<Plan> generate_select_plan(std::shared_ptr<Query> query, Context *context);


    // int get_indexNo(std::string tab_name, std::vector<Condition> curr_conds);
    std::shared_ptr<ScanPlan> make_scan_plan(const TableBinding &binding, std::vector<Condition> conds,
                                             std::vector<TabCol> required_cols = {});

    ColMeta lookup_col_meta(const Query &query, const TabCol &col) const;

    size_t estimate_input_rows(const std::shared_ptr<Plan> &plan) const;

    bool should_use_sort_aggregation(const Query &query, const std::shared_ptr<Plan> &plan,
                                     const std::vector<SortKeySpec> &sort_keys) const;

    ColType interp_sv_type(ast::SvType sv_type) {
        std::map<ast::SvType, ColType> m = {
            {ast::SV_TYPE_INT, TYPE_INT},
            {ast::SV_TYPE_FLOAT, TYPE_FLOAT},
            {ast::SV_TYPE_STRING, TYPE_STRING},
            {ast::SV_TYPE_DATETIME, TYPE_DATETIME}};
        return m.at(sv_type);
    }
};
