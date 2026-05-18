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
#include <optional>
#include <string>
#include <vector>
#include "parser/ast.h"

#include "parser/parser.h"
#include "system/sm_manager.h"
#include "system/sm_meta.h"

typedef enum PlanTag{
    T_Invalid = 1,
    T_Help,
    T_ShowTable,
    T_DescTable,
    T_ShowIndex,
    T_CreateTable,
    T_DropTable,
    T_CreateIndex,
    T_DropIndex,
    T_SetKnob,
    T_Insert,
    T_Update,
    T_Delete,
    T_select,
    T_Transaction_begin,
    T_Transaction_commit,
    T_Transaction_abort,
    T_Transaction_rollback,
    T_SeqScan,
    T_IndexScan,
    T_Join,
    T_NestLoop,
    T_SortMerge,    // sort merge join
    T_HashJoin,     // hash join
    T_Sort,
    T_Projection,
    T_Aggregation
} PlanTag;

enum AggStrategy {
    AggStrategy_Hash,
    AggStrategy_Sort
};

// 查询执行计划
class Plan
{
public:
    PlanTag tag;
    virtual ~Plan() = default;
};

class ScanPlan : public Plan
{
    public:
        ScanPlan(SmManager *sm_manager, std::string tab_name, std::vector<Condition> all_conds,
                 bool empty_result = false)
        {
            Plan::tag = T_SeqScan;
            tab_name_ = std::move(tab_name);
            TabMeta &tab = sm_manager->db_.get_table(tab_name_);
            cols_ = tab.cols;
            len_ = cols_.back().offset + cols_.back().len;
            all_conds_ = std::move(all_conds);
            empty_result_ = empty_result;
        }

        ScanPlan(SmManager *sm_manager, std::string tab_name, std::vector<Condition> index_lookup_conds,
                 std::vector<Condition> residual_conds, std::vector<std::string> index_col_names,
                 std::optional<IndexMeta> index_meta = std::nullopt)
        {
            Plan::tag = T_IndexScan;
            tab_name_ = std::move(tab_name);
            TabMeta &tab = sm_manager->db_.get_table(tab_name_);
            cols_ = tab.cols;
            len_ = cols_.back().offset + cols_.back().len;
            index_lookup_conds_ = std::move(index_lookup_conds);
            residual_conds_ = std::move(residual_conds);
            all_conds_ = index_lookup_conds_;
            all_conds_.insert(all_conds_.end(), residual_conds_.begin(), residual_conds_.end());
            index_col_names_ = std::move(index_col_names);
            if (index_meta.has_value()) {
                index_meta_ = std::move(index_meta);
            } else if (!index_col_names_.empty()) {
                index_meta_ = *tab.get_index_meta(index_col_names_);
            }
        }
        ~ScanPlan(){}
        // 以下变量同ScanExecutor中的变量
        std::string tab_name_;                     
        std::vector<ColMeta> cols_;                
        std::vector<Condition> index_lookup_conds_;
        std::vector<Condition> residual_conds_;
        std::vector<Condition> all_conds_;
        size_t len_;                               
        std::vector<std::string> index_col_names_;
        std::optional<IndexMeta> index_meta_;
        bool empty_result_ = false;
    
};

class JoinPlan : public Plan
{
    public:
        JoinPlan(std::shared_ptr<Plan> left, std::shared_ptr<Plan> right, std::vector<Condition> conds,
                 JoinType join_type = INNER_JOIN)
        {
            Plan::tag = T_Join;
            left_ = std::move(left);
            right_ = std::move(right);
            conds_ = std::move(conds);
            join_type_ = join_type;
        }
        ~JoinPlan(){}
        // Logical join children before physicalization.
        std::shared_ptr<Plan> left_;
        std::shared_ptr<Plan> right_;
        // Logical join predicates collected during join tree construction.
        std::vector<Condition> conds_;
        // The logical join type must survive physicalization because output schema depends on it.
        JoinType join_type_;
};

class PhysicalJoinPlan : public Plan
{
    public:
        PhysicalJoinPlan(PlanTag join_tag, std::shared_ptr<Plan> left, std::shared_ptr<Plan> right,
                         JoinType join_type = INNER_JOIN)
        {
            Plan::tag = join_tag;
            left_ = std::move(left);
            right_ = std::move(right);
            join_type_ = join_type;
        }
        ~PhysicalJoinPlan() override = default;
        // Physical join nodes consume already-implemented children.
        std::shared_ptr<Plan> left_;
        std::shared_ptr<Plan> right_;
        JoinType join_type_;
};

class NestedLoopJoinPlan : public PhysicalJoinPlan
{
    public:
        NestedLoopJoinPlan(std::shared_ptr<Plan> left, std::shared_ptr<Plan> right, std::vector<Condition> conds,
                           JoinType join_type = INNER_JOIN)
            : PhysicalJoinPlan(T_NestLoop, std::move(left), std::move(right), join_type)
        {
            conds_ = std::move(conds);
        }
        ~NestedLoopJoinPlan() override = default;
        // Nested Loop Join re-checks the full predicate list on each candidate pair.
        std::vector<Condition> conds_;
};

class SortMergeJoinPlan : public PhysicalJoinPlan
{
    public:
        SortMergeJoinPlan(std::shared_ptr<Plan> left, std::shared_ptr<Plan> right,
                          std::vector<Condition> merge_conds, std::vector<Condition> residual_conds,
                          JoinType join_type = INNER_JOIN)
            : PhysicalJoinPlan(T_SortMerge, std::move(left), std::move(right), join_type)
        {
            merge_conds_ = std::move(merge_conds);
            residual_conds_ = std::move(residual_conds);
        }
        ~SortMergeJoinPlan() override = default;
        // Equi-join keys are used to drive the merge phase after the planner injects SortPlan nodes.
        std::vector<Condition> merge_conds_;
        std::vector<Condition> residual_conds_;
};

class HashJoinPlan : public PhysicalJoinPlan
{
    public:
        HashJoinPlan(std::shared_ptr<Plan> left, std::shared_ptr<Plan> right,
                     std::vector<Condition> hash_conds, std::vector<Condition> residual_conds,
                     JoinType join_type = INNER_JOIN)
            : PhysicalJoinPlan(T_HashJoin, std::move(left), std::move(right), join_type)
        {
            hash_conds_ = std::move(hash_conds);
            residual_conds_ = std::move(residual_conds);
        }
        ~HashJoinPlan() override = default;
        // Hash Join will build hash tables from equi-join keys when the executor is introduced later.
        std::vector<Condition> hash_conds_;
        std::vector<Condition> residual_conds_;
};

class ProjectionPlan : public Plan
{
    public:
        ProjectionPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::vector<TabCol> sel_cols)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            sel_cols_ = std::move(sel_cols);
        }
        ~ProjectionPlan(){}
        std::shared_ptr<Plan> subplan_;
        std::vector<TabCol> sel_cols_;
        
};

class SortPlan : public Plan
{
    public:
        SortPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::vector<SortKeySpec> sort_keys)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            sort_keys_ = std::move(sort_keys);
        }
        SortPlan(PlanTag tag, std::shared_ptr<Plan> subplan, SortKeySpec sort_key)
            : SortPlan(tag, std::move(subplan), std::vector<SortKeySpec>{std::move(sort_key)}) {}
        ~SortPlan(){}
        std::shared_ptr<Plan> subplan_;
        std::vector<SortKeySpec> sort_keys_;
        
};

class AggregationPlan : public Plan
{
    public:
        AggregationPlan(std::shared_ptr<Plan> subplan, std::vector<AggInfo> agg_infos,
                        std::vector<TabCol> group_by_cols, std::vector<HavingCond> having_conds,
                        AggStrategy strategy = AggStrategy_Hash,
                        std::vector<SortKeySpec> sort_keys = {})
        {
            Plan::tag = T_Aggregation;
            strategy_ = strategy;
            subplan_ = std::move(subplan);
            agg_infos_ = std::move(agg_infos);
            group_by_cols_ = std::move(group_by_cols);
            having_conds_ = std::move(having_conds);
            sort_keys_ = std::move(sort_keys);
        }
        ~AggregationPlan(){}
        AggStrategy strategy_;
        std::shared_ptr<Plan> subplan_;
        std::vector<AggInfo> agg_infos_;
        std::vector<TabCol> group_by_cols_;
        std::vector<HavingCond> having_conds_;
        std::vector<SortKeySpec> sort_keys_;
};

// dml语句，包括insert; delete; update; select语句　
class DMLPlan : public Plan
{
    public:
        DMLPlan(PlanTag tag, std::shared_ptr<Plan> subplan,std::string tab_name,
                std::vector<Value> values, std::vector<Condition> conds,
                std::vector<SetClause> set_clauses)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            tab_name_ = std::move(tab_name);
            values_ = std::move(values);
            conds_ = std::move(conds);
            set_clauses_ = std::move(set_clauses);
        }
        ~DMLPlan(){}
        std::shared_ptr<Plan> subplan_;
        std::string tab_name_;
        std::vector<Value> values_;
        std::vector<Condition> conds_;
        std::vector<SetClause> set_clauses_;
};

// ddl语句, 包括create/drop table; create/drop index;
class DDLPlan : public Plan
{
    public:
        DDLPlan(PlanTag tag, std::string tab_name, std::vector<std::string> col_names,
                std::vector<ColDef> cols, std::vector<IndexSpec> index_specs = {}, bool unique = false)
        {
            Plan::tag = tag;
            tab_name_ = std::move(tab_name);
            cols_ = std::move(cols);
            tab_col_names_ = std::move(col_names);
            index_specs_ = std::move(index_specs);
            unique_ = unique;
        }
        ~DDLPlan(){}
        std::string tab_name_;
        std::vector<std::string> tab_col_names_;
        std::vector<ColDef> cols_;
        std::vector<IndexSpec> index_specs_;
        bool unique_;
};

// help; show tables; desc tables; begin; abort; commit; rollback语句对应的plan
class OtherPlan : public Plan
{
    public:
        OtherPlan(PlanTag tag, std::string tab_name)
        {
            Plan::tag = tag;
            tab_name_ = std::move(tab_name);            
        }
        ~OtherPlan(){}
        std::string tab_name_;
};

// Set Knob Plan
class SetKnobPlan : public Plan
{
    public:
        SetKnobPlan(ast::SetKnobType knob_type, bool bool_value) {
            Plan::tag = T_SetKnob;
            set_knob_type_ = knob_type;
            bool_value_ = bool_value;
        }
    ast::SetKnobType set_knob_type_;
    bool bool_value_;
};

class plannerInfo{
    public:
    std::shared_ptr<ast::SelectStmt> parse;
    std::vector<Condition> where_conds;
    std::vector<TabCol> sel_cols;
    std::shared_ptr<Plan> plan;
    std::vector<std::shared_ptr<Plan>> table_scan_executors;
    std::vector<SetClause> set_clauses;
    plannerInfo(std::shared_ptr<ast::SelectStmt> parse_):parse(std::move(parse_)){}

};
