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
#include <atomic>
#include <memory>
#include <optional>
#include <set>

#include "index_matcher.h"
#include "parser/ast.h"
#include "predicate_normalizer.h"

namespace {

// ================================================================
// Local helper types （Join concerning, ColumnCollector)
// ================================================================

enum class JoinImplementation {
    NestedLoop,
    IndexNestedLoop,
    SortMerge,
    Hash
};

struct JoinPredicateAnalysis {
    std::vector<Condition> all_conds;
    std::vector<Condition> equi_conds;
};

struct JoinImplementationConfig {
    bool enable_nestedloop = true;
    bool enable_index_nestedloop = true;
    bool enable_sortmerge = false;
    bool enable_hash = false;
};

struct JoinImplementationDecision {
    JoinImplementation implementation = JoinImplementation::NestedLoop;
};

struct JoinSubtree {
    std::shared_ptr<Plan> plan;
    std::set<std::string> tables;
};

struct SingleTablePlanInfo {
    std::string table_name;
    std::vector<Condition> conds;
};

struct IndexNestedLoopJoinCandidate {
    Condition index_cond;
    std::shared_ptr<Plan> right_plan;
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

    void collect_select_items(const std::vector<TabCol> &cols) {
        collect_cols(cols);
    }

    void collect_aggregates(const std::vector<AggInfo> &aggs) {
        for (const auto &agg : aggs) {
            if (!agg.is_star) {
                collect_col(agg.col);
            }
        }
    }

    void collect_grouping(const std::vector<TabCol> &cols) {
        collect_cols(cols);
    }

    void collect_ordering(const std::vector<SortKeySpec> &sort_keys) {
        for (const auto &sort_key : sort_keys) {
            collect_col(sort_key.col);
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

    auto columns_for_table(const std::string &table_name) const -> std::vector<TabCol> {
        auto it = table_cols_.find(table_name);
        if (it == table_cols_.end()) {
            return {};
        }
        return it->second;
    }

   private:
    void collect_cols(const std::vector<TabCol> &cols) {
        for (const auto &col : cols) {
            collect_col(col);
        }
    }

    std::map<std::string, std::vector<TabCol>> table_cols_;
};

// ================================================================
// Scan helpers
// ================================================================

void prepare_index_lookup_values(const IndexMeta &index_meta, std::vector<Condition> &lookup_conds) {
    for (auto &cond : lookup_conds) {
        if (!cond.is_rhs_val || cond.rhs_val.raw) {
            continue;
        }
        auto col_it = std::find_if(index_meta.cols.begin(), index_meta.cols.end(),
                                   [&](const ColMeta &col) { return col.name == cond.lhs_col.col_name; });
        if (col_it != index_meta.cols.end() && cond.rhs_val.type == col_it->type) {
            cond.rhs_val.init_raw(col_it->len);
        }
    }
}

auto build_covered_cols(const TabMeta &tab, const std::vector<TabCol> &required_cols) -> std::vector<ColMeta> {
    std::vector<ColMeta> covered_cols;
    covered_cols.reserve(required_cols.size());
    int offset = 0;
    for (const auto &required_col : required_cols) {
        auto col = find_col_meta(tab.cols, required_col);
        col.offset = offset;
        offset += col.len;
        covered_cols.push_back(std::move(col));
    }
    return covered_cols;
}

// ================================================================
// Join expression helpers
// ================================================================

auto convert_join_expr_conds(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds) -> std::vector<Condition> {
    std::vector<Condition> conds;
    conds.reserve(sv_conds.size());
    for (const auto &expr : sv_conds) {
        Condition cond;
        cond.lhs_col = {.tab_name = expr->lhs->tab_name, .col_name = expr->lhs->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.rhs_val = convert_sv_value(rhs_val);
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

// ================================================================
// Join tree helpers
// ================================================================

auto contains_table(const JoinSubtree &subtree, const std::string &table_name) -> bool {
    return subtree.tables.find(table_name) != subtree.tables.end();
}

auto find_subtree_index(const std::vector<JoinSubtree> &subtrees,
                        const std::string &table_name) -> std::optional<size_t> {
    for (size_t i = 0; i < subtrees.size(); ++i) {
        if (contains_table(subtrees[i], table_name)) {
            return i;
        }
    }
    return std::nullopt;
}

auto take_subtree(std::vector<JoinSubtree> &subtrees, size_t index) -> JoinSubtree {
    auto subtree = std::move(subtrees[index]);
    subtrees.erase(subtrees.begin() + static_cast<std::ptrdiff_t>(index));
    return subtree;
}

auto take_subtree_pair(std::vector<JoinSubtree> &subtrees, size_t left_idx, size_t right_idx)
    -> std::pair<JoinSubtree, JoinSubtree> {
    if (left_idx < right_idx) {
        auto right = take_subtree(subtrees, right_idx);
        auto left = take_subtree(subtrees, left_idx);
        return {std::move(left), std::move(right)};
    }
    auto left = take_subtree(subtrees, left_idx);
    auto right = take_subtree(subtrees, right_idx);
    return {std::move(left), std::move(right)};
}

auto build_join_subtrees(std::vector<std::pair<std::string, std::shared_ptr<Plan>>> &table_plans)
    -> std::vector<JoinSubtree> {
    std::vector<JoinSubtree> subtrees;
    subtrees.reserve(table_plans.size());
    for (auto &[name, plan] : table_plans) {
        subtrees.push_back(JoinSubtree{std::move(plan), std::set<std::string>{name}});
    }
    return subtrees;
}

auto condition_spans_subtrees(const Condition &cond, const JoinSubtree &left, const JoinSubtree &right) -> bool {
    if (cond.is_rhs_val) {
        return false;
    }
    bool lhs_in_left = contains_table(left, cond.lhs_col.tab_name);
    bool lhs_in_right = contains_table(right, cond.lhs_col.tab_name);
    bool rhs_in_left = contains_table(left, cond.rhs_col.tab_name);
    bool rhs_in_right = contains_table(right, cond.rhs_col.tab_name);
    return (lhs_in_left && rhs_in_right) || (lhs_in_right && rhs_in_left);
}

auto condition_tables_in_subtree(const Condition &cond, const JoinSubtree &subtree) -> bool {
    if (cond.is_rhs_val) {
        return contains_table(subtree, cond.lhs_col.tab_name);
    }
    return contains_table(subtree, cond.lhs_col.tab_name) && contains_table(subtree, cond.rhs_col.tab_name);
}

auto orient_condition_for_join(const Condition &cond, const JoinSubtree &left, const JoinSubtree &right)
    -> Condition {
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
        return oriented;
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

auto collect_join_conditions(std::vector<Condition> direct_conds, std::vector<Condition> &pending_conds,
                             JoinSubtree &left, JoinSubtree &right) -> std::vector<Condition> {
    std::vector<Condition> join_conds;
    join_conds.reserve(direct_conds.size() + pending_conds.size());
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

auto make_join_subtree(JoinSubtree left, JoinSubtree right, std::vector<Condition> conds, JoinType join_type)
    -> JoinSubtree {
    auto tables = std::move(left.tables);
    tables.insert(right.tables.begin(), right.tables.end());
    auto plan = std::make_shared<JoinPlan>(std::move(left.plan), std::move(right.plan),
                                           std::move(conds), join_type);
    return JoinSubtree{std::move(plan), std::move(tables)};
}

auto find_pending_join_subtrees(const std::vector<JoinSubtree> &subtrees, const Condition &cond)
    -> std::optional<std::pair<size_t, size_t>> {
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

auto join_from_pending_conditions(std::vector<JoinSubtree> &subtrees, std::vector<Condition> &pending_conds)
    -> bool {
    std::optional<std::pair<size_t, size_t>> selected_indexes;
    for (size_t i = 0; i < pending_conds.size(); ++i) {
        auto indexes = find_pending_join_subtrees(subtrees, pending_conds[i]);
        if (!indexes.has_value()) {
            continue;
        }
        selected_indexes = indexes;
        break;
    }
    if (!selected_indexes.has_value()) {
        return false;
    }
    auto [left_idx, right_idx] = *selected_indexes;
    auto [left, right] = take_subtree_pair(subtrees, left_idx, right_idx);
    auto join_conds = collect_join_conditions(std::vector<Condition>(), pending_conds, left, right);
    subtrees.push_back(make_join_subtree(std::move(left), std::move(right),
                                         std::move(join_conds), INNER_JOIN));
    return true;
}

// ================================================================
// Join predicate helpers
// ================================================================

auto analyze_join_predicates(const std::vector<Condition> &conds) -> JoinPredicateAnalysis {
    JoinPredicateAnalysis analysis;
    analysis.all_conds = conds;
    for (const auto &cond : conds) {
        if (cond.is_rhs_val || cond.op != OP_EQ) {
            continue;
        }
        analysis.equi_conds.push_back(cond);
    }
    return analysis;
}

auto join_has_equi_keys(const JoinPredicateAnalysis &analysis) -> bool {
    return !analysis.equi_conds.empty();
}

auto join_residual_conds(const JoinPredicateAnalysis &analysis) -> std::vector<Condition> {
    std::vector<Condition> residual_conds = analysis.all_conds;
    if (join_has_equi_keys(analysis)) {
        residual_conds.erase(
            std::remove_if(residual_conds.begin(), residual_conds.end(),
                           [](const Condition &cond) { return !cond.is_rhs_val && cond.op == OP_EQ; }),
            residual_conds.end());
    }
    return residual_conds;
}

auto join_left_key_cols(const JoinPredicateAnalysis &analysis) -> std::vector<TabCol> {
    std::vector<TabCol> cols;
    cols.reserve(analysis.equi_conds.size());
    for (const auto &cond : analysis.equi_conds) {
        cols.push_back(cond.lhs_col);
    }
    return cols;
}

auto join_right_key_cols(const JoinPredicateAnalysis &analysis) -> std::vector<TabCol> {
    std::vector<TabCol> cols;
    cols.reserve(analysis.equi_conds.size());
    for (const auto &cond : analysis.equi_conds) {
        cols.push_back(cond.rhs_col);
    }
    return cols;
}

auto append_conditions(std::vector<Condition> &target, const std::vector<Condition> &source) -> void {
    target.insert(target.end(), source.begin(), source.end());
}

auto extract_single_table_plan_info(const std::shared_ptr<Plan> &plan) -> std::optional<SingleTablePlanInfo> {
    if (auto projection = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        return extract_single_table_plan_info(projection->subplan_);
    }
    if (auto filter = std::dynamic_pointer_cast<FilterPlan>(plan)) {
        auto info = extract_single_table_plan_info(filter->subplan_);
        if (!info.has_value()) {
            return std::nullopt;
        }
        append_conditions(info->conds, filter->conds_);
        return info;
    }
    if (auto scan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        SingleTablePlanInfo info;
        info.table_name = scan->tab_name_;
        append_conditions(info.conds, scan->all_conds_);
        append_conditions(info.conds, scan->access_conds_);
        append_conditions(info.conds, scan->residual_conds_);
        return info;
    }
    return std::nullopt;
}

auto find_runtime_bindable_index_scan(const std::shared_ptr<Plan> &plan) -> std::shared_ptr<ScanPlan> {
    if (auto projection = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        return find_runtime_bindable_index_scan(projection->subplan_);
    }
    if (auto filter = std::dynamic_pointer_cast<FilterPlan>(plan)) {
        return find_runtime_bindable_index_scan(filter->subplan_);
    }
    auto scan = std::dynamic_pointer_cast<ScanPlan>(plan);
    if (scan == nullptr || scan->tag != T_IndexScan) {
        return nullptr;
    }
    return scan;
}

auto index_scan_supports_runtime_probe(const ScanPlan &scan, const TabCol &lookup_col) -> bool {
    return std::any_of(scan.access_conds_.begin(), scan.access_conds_.end(), [&](const Condition &cond) {
        return cond.is_rhs_val && cond.op == OP_EQ && cond.lhs_col.equals(lookup_col);
    });
}

auto find_index_nestedloop_join_candidate(const JoinPredicateAnalysis &analysis, const std::shared_ptr<Plan> &right)
    -> std::optional<IndexNestedLoopJoinCandidate> {
    auto right_info = extract_single_table_plan_info(right);
    if (!right_info.has_value()) {
        return std::nullopt;
    }
    auto right_index_scan = find_runtime_bindable_index_scan(right);
    if (right_index_scan == nullptr || right_index_scan->tab_name_ != right_info->table_name) {
        return std::nullopt;
    }

    for (const auto &cond : analysis.equi_conds) {
        if (cond.is_rhs_val || cond.rhs_col.tab_name != right_info->table_name) {
            continue;
        }
        if (!index_scan_supports_runtime_probe(*right_index_scan, cond.rhs_col)) {
            continue;
        }
        return IndexNestedLoopJoinCandidate{cond, right};
    }
    return std::nullopt;
}

// ================================================================
// Join implementation helpers
// ================================================================

auto build_join_implementation_config(bool enable_nestedloop_join, bool enable_index_nestedloop_join,
                                      bool enable_sortmerge_join, bool enable_hash_join)
    -> JoinImplementationConfig {
    JoinImplementationConfig config;
    config.enable_nestedloop = enable_nestedloop_join;
    config.enable_index_nestedloop = enable_index_nestedloop_join;
    config.enable_sortmerge = enable_sortmerge_join;
    config.enable_hash = enable_hash_join;
    return config;
}

void validate_join_executor_config(const JoinImplementationConfig &config) {
    if (!config.enable_nestedloop && !config.enable_index_nestedloop &&
        !config.enable_sortmerge && !config.enable_hash) {
        throw RMDBError("No join executor selected!");
    }
}

auto supports_join_implementation(JoinImplementation implementation, JoinType join_type) -> bool {
    switch (implementation) {
        case JoinImplementation::NestedLoop:
            return true;
        case JoinImplementation::IndexNestedLoop:
            break;
        case JoinImplementation::SortMerge:
        case JoinImplementation::Hash:
            break;
    }
    switch (join_type) {
        case INNER_JOIN:
        case SEMI_JOIN:
            return true;
        default:
            return false;
    }
}

auto choose_join_implementation(const JoinPredicateAnalysis &analysis, JoinType join_type,
                                const JoinImplementationConfig &config,
                                bool has_index_nestedloop_candidate) -> JoinImplementationDecision {
    JoinImplementationDecision decision;
    if (config.enable_index_nestedloop && has_index_nestedloop_candidate &&
        supports_join_implementation(JoinImplementation::IndexNestedLoop, join_type)) {
        decision.implementation = JoinImplementation::IndexNestedLoop;
        return decision;
    }
    if (config.enable_hash && join_has_equi_keys(analysis) &&
        supports_join_implementation(JoinImplementation::Hash, join_type)) {
        decision.implementation = JoinImplementation::Hash;
        return decision;
    }
    if (config.enable_sortmerge && join_has_equi_keys(analysis) &&
        supports_join_implementation(JoinImplementation::SortMerge, join_type)) {
        decision.implementation = JoinImplementation::SortMerge;
        return decision;
    }
    if (config.enable_nestedloop) {
        decision.implementation = JoinImplementation::NestedLoop;
        return decision;
    }
    throw RMDBError("No join implementation available!");
}

// ================================================================
// Join plan helpers
// ================================================================

auto build_nestedloop_join_plan(std::shared_ptr<Plan> left, std::shared_ptr<Plan> right,
                                const JoinPredicateAnalysis &analysis, JoinType join_type)
    -> std::shared_ptr<Plan> {
    return std::make_shared<NestedLoopJoinPlan>(std::move(left), std::move(right), analysis.all_conds, join_type);
}

auto build_index_nestedloop_join_plan(std::shared_ptr<Plan> left, std::shared_ptr<Plan> right,
                                      const JoinPredicateAnalysis &analysis, JoinType join_type,
                                      const IndexNestedLoopJoinCandidate &candidate) -> std::shared_ptr<Plan> {
    return std::make_shared<IndexNestedLoopJoinPlan>(std::move(left), std::move(right), analysis.all_conds,
                                                     candidate.index_cond.lhs_col, candidate.index_cond.rhs_col,
                                                     join_type);
}

auto build_sortmerge_join_plan(std::shared_ptr<Plan> left, std::shared_ptr<Plan> right,
                               const JoinPredicateAnalysis &analysis, JoinType join_type)
    -> std::shared_ptr<Plan> {
    auto sorted_left = std::make_shared<SortPlan>(T_Sort, std::move(left),
                                                  make_sort_key_specs(join_left_key_cols(analysis)));
    auto sorted_right = std::make_shared<SortPlan>(T_Sort, std::move(right),
                                                   make_sort_key_specs(join_right_key_cols(analysis)));
    return std::make_shared<SortMergeJoinPlan>(std::move(sorted_left), std::move(sorted_right),
                                               analysis.equi_conds, join_residual_conds(analysis), join_type);
}

auto build_hash_join_plan(std::shared_ptr<Plan> left, std::shared_ptr<Plan> right,
                          const JoinPredicateAnalysis &analysis, JoinType join_type)
    -> std::shared_ptr<Plan> {
    return std::make_shared<HashJoinPlan>(std::move(left), std::move(right), analysis.equi_conds,
                                          join_residual_conds(analysis), join_type);
}

auto physicalize_logical_join(const std::shared_ptr<JoinPlan> &join, std::shared_ptr<Plan> left,
                              std::shared_ptr<Plan> right, const JoinImplementationConfig &config)
    -> std::shared_ptr<Plan> {
    auto analysis = analyze_join_predicates(join->conds_);
    auto index_nestedloop_candidate = find_index_nestedloop_join_candidate(analysis, right);
    auto decision = choose_join_implementation(analysis, join->join_type_, config,
                                               index_nestedloop_candidate.has_value());
    switch (decision.implementation) {
        case JoinImplementation::NestedLoop:
            return build_nestedloop_join_plan(std::move(left), std::move(right), analysis, join->join_type_);
        case JoinImplementation::IndexNestedLoop:
            if (!index_nestedloop_candidate.has_value()) {
                throw InternalError("Index nested loop join selected without a candidate");
            }
            return build_index_nestedloop_join_plan(std::move(left), std::move(index_nestedloop_candidate->right_plan), analysis,
                                                    join->join_type_, *index_nestedloop_candidate);
        case JoinImplementation::SortMerge:
            return build_sortmerge_join_plan(std::move(left), std::move(right), analysis, join->join_type_);
        case JoinImplementation::Hash:
            return build_hash_join_plan(std::move(left), std::move(right), analysis, join->join_type_);
    }
    throw InternalError("Unexpected join implementation decision");
}

auto physicalize_join_tree(const std::shared_ptr<Plan> &plan, const JoinImplementationConfig &config, int depth = 0)
    -> std::shared_ptr<Plan> {
    if (depth >= kMaxPlanTreeDepth) {
        throw InternalError("Plan tree depth exceeds limit");
    }
    auto join = std::dynamic_pointer_cast<JoinPlan>(plan);
    if (join == nullptr) {
        return plan;
    }

    // Children must be physicalized before the parent join is built.
    auto left = physicalize_join_tree(join->left_, config, depth + 1);
    auto right = physicalize_join_tree(join->right_, config, depth + 1);
    return physicalize_logical_join(join, std::move(left), std::move(right), config);
}

// ================================================================
// Ordering helpers
// ================================================================

auto ordering_has_required_prefix(const std::vector<SortKeySpec> &ordering,
                                  const std::vector<SortKeySpec> &required_prefix) -> bool {
    if (ordering.size() < required_prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < required_prefix.size(); ++i) {
        if (!ordering[i].equals(required_prefix[i])) {
            return false;
        }
    }
    return true;
}

auto derive_plan_ordering(const std::shared_ptr<Plan> &plan) -> std::vector<SortKeySpec> {
    if (plan == nullptr) {
        return {};
    }
    if (auto sort = std::dynamic_pointer_cast<SortPlan>(plan)) {
        return sort->sort_keys_;
    }
    if (auto filter = std::dynamic_pointer_cast<FilterPlan>(plan)) {
        return derive_plan_ordering(filter->subplan_);
    }
    if (auto projection = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        return derive_plan_ordering(projection->subplan_);
    }
    if (auto limit = std::dynamic_pointer_cast<LimitPlan>(plan)) {
        return derive_plan_ordering(limit->subplan_);
    }
    if (auto aggregation = std::dynamic_pointer_cast<AggregationPlan>(plan);
        aggregation != nullptr && aggregation->strategy_ == AggStrategy_Sort) {
        return make_sort_key_specs(aggregation->group_by_cols_);
    }
    return {};
}

auto plan_provides_ordering(const std::shared_ptr<Plan> &plan,
                            const std::vector<SortKeySpec> &required_keys) -> bool {
    if (required_keys.empty()) {
        return true;
    }
    return ordering_has_required_prefix(derive_plan_ordering(plan), required_keys);
}

auto order_by_compatible_with_grouping(const Query &query,
                                        const std::vector<SortKeySpec> &group_sort_keys) -> bool {
    return !query.order_by_keys.empty() && ordering_has_required_prefix(group_sort_keys, query.order_by_keys);
}

auto is_aggregate_query(const Query &query) -> bool {
    return !query.agg_infos.empty() || !query.group_by_cols.empty() || !query.having_conds.empty();
}

}  // namespace

Planner::ScanBuildResult Planner::make_scan_plan(const std::string &tab_name,
                                                 const std::vector<Condition> &semantic_conds,
                                                 std::vector<TabCol> required_cols,
                                                 bool allow_covering_index) {
    ScanBuildResult result;
    const auto &tab = sm_manager_->db_.get_table(tab_name);
    auto best_match = match_best_index(tab, semantic_conds, required_cols);
    if (!best_match.matched) {
        result.scan = std::make_shared<ScanPlan>(sm_manager_, tab_name, std::vector<Condition>());
        result.filter_conds = semantic_conds;
        return result;
    }
    if (best_match.index_meta.has_value()) {
        prepare_index_lookup_values(*best_match.index_meta, best_match.lookup_conds);
    }
    result.scan = std::make_shared<ScanPlan>(sm_manager_, tab_name,
                                             std::move(best_match.lookup_conds),
                                             std::move(best_match.residual_conds),
                                             std::move(best_match.index_col_names),
                                             std::move(best_match.index_meta));
    if (allow_covering_index && best_match.covers_required_cols && best_match.covers_residual_conds) {
        result.scan->enable_covering_index(build_covered_cols(tab, required_cols));
    }
    return result;
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
    auto scan_result = make_scan_plan(tab_name, table_conds, std::move(required_cols), false);
    if (plan_context.empty_tables.count(tab_name) != 0) {
        scan_result.scan->empty_result_ = true;
    }
    if (scan_result.filter_conds.empty()) {
        return scan_result.scan;
    }
    return std::make_shared<FilterPlan>(std::move(scan_result.scan),
                                        std::move(scan_result.filter_conds));
}

Planner::LogicalPlanContext Planner::logical_optimization(const std::shared_ptr<Query> &query, Context *context)
{
    LogicalPlanContext plan_context;
    std::map<std::string, std::vector<Condition>> raw_table_conds;
    ColumnUsageCollector column_usage;

    auto classify_condition = [&](const Condition &cond, std::vector<Condition> *join_conds) {
        plan_context.all_conds.push_back(cond);
        column_usage.collect_condition(cond);
        if (cond.is_rhs_val) {
            raw_table_conds[cond.lhs_col.tab_name].push_back(cond);
        } else if (cond.lhs_col.tab_name == cond.rhs_col.tab_name) {
            raw_table_conds[cond.lhs_col.tab_name].push_back(cond);
        } else if (join_conds != nullptr) {
            join_conds->push_back(cond);
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
    }

    for (auto &[tab_name, conds] : raw_table_conds) {
        const auto &table_cols = sm_manager_->db_.get_table(tab_name).cols;
        auto normalized = normalize_predicates(table_cols, conds);
        if (normalized.contradiction) {
            plan_context.table_conds[tab_name].clear();
            plan_context.empty_tables.insert(tab_name);
        } else {
            plan_context.table_conds[tab_name] = std::move(normalized.normalized_conds);
        }
    }

    column_usage.collect_select_items(query->cols);
    column_usage.collect_aggregates(query->agg_infos);
    column_usage.collect_grouping(query->group_by_cols);
    column_usage.collect_ordering(query->order_by_keys);
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
    if (!is_aggregate_query(*query)) {
        plan = generate_sort_plan(query, std::move(plan));
    }

    return plan;
}


/**
 * @brief: 根据plan_context中的条件和所需列，为每个表构建扫描计划
 * @param query: 查询对象
 * @param plan_context: 计划上下文
 * @return: 每个表的扫描计划
 */
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
        auto scan_result = make_scan_plan(tab_name, curr_conds, required_cols, true);
        if (plan_context.empty_tables.count(tab_name) != 0) {
            scan_result.scan->empty_result_ = true;
        }
        std::shared_ptr<Plan> node = std::make_shared<FilterPlan>(std::move(scan_result.scan),
                                                                  std::move(scan_result.filter_conds));
        // Insert projection pushdown when column pruning is beneficial
        const auto &table_cols = sm_manager_->db_.get_table(tab_name).cols;
        //  required_cols < table_cols 时裁剪列 投影下推
        if (required_cols.size() < table_cols.size()) {
            std::vector<std::string> proj_output_names;
            proj_output_names.reserve(required_cols.size());
            for (const auto &col : required_cols) {
                proj_output_names.push_back(col.col_name);
            }
            node = std::make_shared<ProjectionPlan>(T_Projection, std::move(node),
                                                    std::move(required_cols),
                                                    std::move(proj_output_names));
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

    // Explicit jointree from JOIN ... ON clauses.
    for (const auto &join_expr : jointree) {
        auto left_idx = find_subtree_index(subtrees, join_expr.left);
        auto right_idx = find_subtree_index(subtrees, join_expr.right);
        if (!left_idx.has_value() || !right_idx.has_value()) {
            throw InternalError("Explicit join input is not part of the query");
        }
        if (left_idx == right_idx) {
            throw InternalError("Explicit join inputs already belong to the same subtree");
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

/**
 * @brief: 根据plan_context中的条件和所需列，为每个表构建扫描计划，并返回一个关系计划
 * @param query: 查询对象
 * @param plan_context: 计划上下文
 * @return: 关系计划
 */
std::shared_ptr<Plan> Planner::make_one_rel(std::shared_ptr<Query> query, const LogicalPlanContext &plan_context)
{
    auto table_plans = build_table_plans(query, plan_context);

    if (table_plans.size() == 1) {
        return table_plans[0].second;
    }

    auto plan = build_join_tree(table_plans, plan_context.join_conds, plan_context.explicit_joins);

    auto join_impl_config = build_join_implementation_config(enable_nestedloop_join,
                                                             enable_nestedloop_join,
                                                             enable_sortmerge_join,
                                                             this->enable_hash_join);
    validate_join_executor_config(join_impl_config);

    return physicalize_join_tree(plan, join_impl_config);
}


std::shared_ptr<Plan> Planner::generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan)
{
    if (query->order_by_keys.empty()) {
        return plan;
    }
    if (plan_provides_ordering(plan, query->order_by_keys)) {
        return plan;
    }
    return std::make_shared<SortPlan>(T_Sort, std::move(plan), query->order_by_keys);
}

std::shared_ptr<Plan> Planner::generate_limit_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan)
{
    if (!query->limit_spec.has_value()) {
        return plan;
    }
    return std::make_shared<LimitPlan>(std::move(plan), *query->limit_spec);
}

ColMeta Planner::lookup_col_meta(const TabCol &col) const {
    const auto &cols = sm_manager_->db_.get_table(col.tab_name).cols;
    return find_col_meta(cols, col);
}

// ================================================================
// aggregation strategy concerning helpers
// ================================================================
size_t Planner::estimate_input_rows(const std::shared_ptr<Plan> &plan) const {
    if (auto scan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        auto fh_it = sm_manager_->fhs_.find(scan->tab_name_);
        if (fh_it == sm_manager_->fhs_.end()) {
            return kDefaultAggregationRows;
        }
        auto hdr = fh_it->second->get_file_hdr();
        int pages = hdr.num_pages.load(std::memory_order_acquire);
        if (pages <= RM_FIRST_RECORD_PAGE) {
            return 0;
        }
        return static_cast<size_t>(pages - RM_FIRST_RECORD_PAGE) *
               static_cast<size_t>(hdr.num_records_per_page);
    }
    if (auto sort = std::dynamic_pointer_cast<SortPlan>(plan)) {
        return estimate_input_rows(sort->subplan_);
    }
    if (auto limit = std::dynamic_pointer_cast<LimitPlan>(plan)) {
        return std::min(estimate_input_rows(limit->subplan_), limit->limit_spec_.limit);
    }
    if (auto projection = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        return estimate_input_rows(projection->subplan_);
    }
    if (auto aggregation = std::dynamic_pointer_cast<AggregationPlan>(plan)) {
        return estimate_input_rows(aggregation->subplan_);
    }
    if (auto join = std::dynamic_pointer_cast<PhysicalJoinPlan>(plan)) {
        return std::max(estimate_input_rows(join->left_), estimate_input_rows(join->right_));
    }
    // Logical joins can expand cardinality; keep the default conservative without table statistics.
    return kDefaultAggregationRows;
}

bool Planner::should_use_sort_aggregation(const Query &query, const std::shared_ptr<Plan> &plan,
                                          const std::vector<SortKeySpec> &sort_keys) const {
    for (const auto &group_col : query.group_by_cols) {
        auto type = lookup_col_meta(group_col).type;
        if (type != TYPE_INT && type != TYPE_FLOAT && type != TYPE_STRING && type != TYPE_DATETIME) {
            return true;
        }
    }

    if (plan_provides_ordering(plan, sort_keys)) {
        return true;
    }

    if (order_by_compatible_with_grouping(query, sort_keys)) {
        return true;
    }

    size_t input_rows = estimate_input_rows(plan);
    size_t key_width = 0;
    for (const auto &group_col : query.group_by_cols) {
        const auto &col = lookup_col_meta(group_col);
        key_width += col.type == TYPE_STRING ? static_cast<size_t>(std::max(1, col.len / 2))
                                             : static_cast<size_t>(col.len);
    }
    size_t agg_width = 0;
    for (const auto &agg : query.agg_infos) {
        switch (agg.agg_type) {
            case AGG_COUNT:    agg_width += sizeof(int); break;
            case AGG_SUM:
            case AGG_AVG:      agg_width += sizeof(double) + sizeof(int); break;
            case AGG_MIN:
            case AGG_MAX:
                agg_width += agg.is_star ? sizeof(int)
                                          : static_cast<size_t>(lookup_col_meta(agg.col).len);
                break;
        }
        agg_width += sizeof(bool);
    }
    size_t estimated_groups = std::min(input_rows, kMaxEstimatedGroups);
    size_t per_group_overhead = 64;
    size_t hash_bytes = estimated_groups * (key_width + agg_width + per_group_overhead);
    double adjusted = static_cast<double>(hash_bytes) * kHashAggregationSafetyFactor;
    return adjusted > static_cast<double>(kHashAggregationMemoryBudgetBytes);
}

std::shared_ptr<Plan> Planner::generate_aggregate_plan(std::shared_ptr<Query> query,
                                                         std::shared_ptr<Plan> plan) {
    if (!is_aggregate_query(*query)) {
        return plan;
    }
    // no group-by keys: scalar aggregate always uses hash
    if (query->group_by_cols.empty()) {
        return std::make_shared<AggregationPlan>(std::move(plan), query->agg_infos,
                                                  query->group_by_cols, query->having_conds,
                                                  AggStrategy_Hash);
    }

    auto sort_keys = make_sort_key_specs(query->group_by_cols);
    if (!should_use_sort_aggregation(*query, plan, sort_keys)) {
        return std::make_shared<AggregationPlan>(std::move(plan), query->agg_infos,
                                                  query->group_by_cols, query->having_conds,
                                                  AggStrategy_Hash);
    }

    if (!plan_provides_ordering(plan, sort_keys)) {
        plan = std::make_shared<SortPlan>(T_Sort, std::move(plan), sort_keys);
    }
    return std::make_shared<AggregationPlan>(std::move(plan), query->agg_infos,
                                              query->group_by_cols, query->having_conds,
                                              AggStrategy_Sort);
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
    auto output_names = query->output_names;
    if (output_names.empty()) {
        output_names.reserve(sel_cols.size());
        for (const auto &sel_col : sel_cols) {
            output_names.push_back(sel_col.col_name);
        }
    }
    std::shared_ptr<Plan> plannerRoot = physical_optimization(query, plan_context, context);

    plannerRoot = generate_aggregate_plan(query, std::move(plannerRoot));

    if (is_aggregate_query(*query)) {
        plannerRoot = generate_sort_plan(query, std::move(plannerRoot));
    }

    plannerRoot = generate_limit_plan(query, std::move(plannerRoot));

    plannerRoot = std::make_shared<ProjectionPlan>(T_Projection, std::move(plannerRoot),
                                                   std::move(sel_cols), std::move(output_names));

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
                                  .type = convert_sv_type(sv_col_def->type_len->type),
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
        // delete — route through logical_optimization for condition normalization
        auto plan_context = logical_optimization(query, context);
        auto filtered_scan = build_dml_scan_plan(plan_context, x->tab_name);

        plannerRoot = std::make_shared<DMLPlan>(T_Delete, filtered_scan, x->tab_name,
                                                std::vector<Value>(), std::vector<Condition>(), std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(query->parse)) {
        // update — route through logical_optimization for condition normalization
        auto plan_context = logical_optimization(query, context);
        auto filtered_scan = build_dml_scan_plan(plan_context, x->tab_name);
        plannerRoot = std::make_shared<DMLPlan>(T_Update, filtered_scan, x->tab_name,
                                                     std::vector<Value>(), std::vector<Condition>(),
                                                     query->set_clauses);
    } else if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {

        std::shared_ptr<plannerInfo> root = std::make_shared<plannerInfo>(x);
        // Save flags before query is moved into generate_select_plan
        bool is_explain_analyze = query->is_explain_analyze;
        auto table_display_names = query->table_display_names;
        bool display_wildcard = x->select_items.empty();
        // 生成select语句的查询执行计划
        std::shared_ptr<Plan> projection = generate_select_plan(std::move(query), context);
        plannerRoot = std::make_shared<DMLPlan>(T_select, projection, std::string(), std::vector<Value>(),
                                                    std::vector<Condition>(), std::vector<SetClause>(),
                                                    is_explain_analyze,
                                                    std::move(table_display_names),
                                                    display_wildcard);
    } else {
        throw InternalError("Unexpected AST root");
    }
    return plannerRoot;
}
