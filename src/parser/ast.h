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

#include <vector>
#include <string>
#include <memory>
#include "common/common.h"

enum JoinType {
    INNER_JOIN, LEFT_JOIN, RIGHT_JOIN, FULL_JOIN, SEMI_JOIN
};
namespace ast {

enum SvType {
    SV_TYPE_INT, SV_TYPE_FLOAT, SV_TYPE_STRING, SV_TYPE_BOOL
};

enum SvCompOp {
    SV_OP_EQ, SV_OP_NE, SV_OP_LT, SV_OP_GT, SV_OP_LE, SV_OP_GE
};

enum OrderByDir {
    OrderBy_DEFAULT,
    OrderBy_ASC,
    OrderBy_DESC
};

enum SetKnobType {
    EnableNestLoop, EnableSortMerge, EnableHashJoin
};

// Base class for tree nodes
struct TreeNode {
    virtual ~TreeNode() = default;  // enable polymorphism
};

struct Help : public TreeNode {
};

struct ShowTables : public TreeNode {
};

struct ShowIndex : public TreeNode {
    std::string tab_name;

    explicit ShowIndex(std::string tab_name_) : tab_name(std::move(tab_name_)) {}
};

struct TxnBegin : public TreeNode {
};

struct TxnCommit : public TreeNode {
};

struct TxnAbort : public TreeNode {
};

struct TxnRollback : public TreeNode {
};

struct TypeLen : public TreeNode {
    SvType type;
    int len;

    TypeLen(SvType type_, int len_) : type(type_), len(len_) {}
};

struct Field : public TreeNode {
};

struct ColDef : public Field {
    std::string col_name;
    std::shared_ptr<TypeLen> type_len;
    bool unique;

    ColDef(std::string col_name_, std::shared_ptr<TypeLen> type_len_, bool unique_ = false) :
            col_name(std::move(col_name_)), type_len(std::move(type_len_)), unique(unique_) {}
};

struct UniqueDef : public Field {
    std::vector<std::string> col_names;

    explicit UniqueDef(std::vector<std::string> col_names_) : col_names(std::move(col_names_)) {}
};

struct CreateTable : public TreeNode {
    std::string tab_name;
    std::vector<std::shared_ptr<Field>> fields;

    CreateTable(std::string tab_name_, std::vector<std::shared_ptr<Field>> fields_) :
            tab_name(std::move(tab_name_)), fields(std::move(fields_)) {}
};

struct DropTable : public TreeNode {
    std::string tab_name;

    DropTable(std::string tab_name_) : tab_name(std::move(tab_name_)) {}
};

struct DescTable : public TreeNode {
    std::string tab_name;

    DescTable(std::string tab_name_) : tab_name(std::move(tab_name_)) {}
};

struct CreateIndex : public TreeNode {
    std::string tab_name;
    std::vector<std::string> col_names;
    bool unique;

    CreateIndex(std::string tab_name_, std::vector<std::string> col_names_, bool unique_ = false) :
            tab_name(std::move(tab_name_)), col_names(std::move(col_names_)), unique(unique_) {}
};

struct DropIndex : public TreeNode {
    std::string tab_name;
    std::vector<std::string> col_names;

    DropIndex(std::string tab_name_, std::vector<std::string> col_names_) :
            tab_name(std::move(tab_name_)), col_names(std::move(col_names_)) {}
};

struct Expr : public TreeNode {
};

struct Value : public Expr {
};

struct Col;

struct IntLit : public Value {
    int val;

    IntLit(int val_) : val(val_) {}
};

struct FloatLit : public Value {
    float val;

    FloatLit(float val_) : val(val_) {}
};

struct StringLit : public Value {
    std::string val;

    StringLit(std::string val_) : val(std::move(val_)) {}
};

struct BoolLit : public Value {
    bool val;

    BoolLit(bool val_) : val(val_) {}
};

struct AggFunc : public Expr {
    AggType agg_type;
    bool is_star;
    std::shared_ptr<Col> col;

    AggFunc(AggType agg_type_, bool is_star_, std::shared_ptr<Col> col_) :
            agg_type(agg_type_), is_star(is_star_), col(std::move(col_)) {}
};

struct Col : public Expr {
    std::string tab_name;
    std::string col_name;

    Col(std::string tab_name_, std::string col_name_) :
            tab_name(std::move(tab_name_)), col_name(std::move(col_name_)) {}
};

struct SelectItem : public TreeNode {
    std::shared_ptr<Expr> expr;
    bool has_alias = false;
    std::string alias;

    explicit SelectItem(std::shared_ptr<Expr> expr_) : expr(std::move(expr_)) {}

    SelectItem(std::shared_ptr<Expr> expr_, std::string alias_) :
            expr(std::move(expr_)), has_alias(true), alias(std::move(alias_)) {}
};


struct SetClause : public TreeNode {
    std::string col_name;
    std::shared_ptr<Value> val;

    SetClause(std::string col_name_, std::shared_ptr<Value> val_) :
            col_name(std::move(col_name_)), val(std::move(val_)) {}
};

struct BinaryExpr : public TreeNode {
    std::shared_ptr<Col> lhs;
    SvCompOp op;
    std::shared_ptr<Expr> rhs;

    BinaryExpr(std::shared_ptr<Col> lhs_, SvCompOp op_, std::shared_ptr<Expr> rhs_) :
            lhs(std::move(lhs_)), op(op_), rhs(std::move(rhs_)) {}
};

struct GroupBy : public TreeNode {
    std::vector<std::shared_ptr<Col>> cols;

    GroupBy(std::vector<std::shared_ptr<Col>> cols_) : cols(std::move(cols_)) {}
};

struct HavingCond : public TreeNode {
    bool is_agg;
    std::shared_ptr<AggFunc> agg;
    std::shared_ptr<Col> col;
    SvCompOp op;
    std::shared_ptr<Expr> rhs;

    HavingCond(bool is_agg_, std::shared_ptr<AggFunc> agg_, std::shared_ptr<Col> col_, SvCompOp op_, std::shared_ptr<Expr> rhs_) :
            is_agg(is_agg_), agg(std::move(agg_)), col(std::move(col_)), op(op_), rhs(std::move(rhs_)) {}
};

struct OrderBy : public TreeNode
{
    struct Item : public TreeNode {
        std::shared_ptr<Expr> expr;
        std::shared_ptr<Col> col;
        OrderByDir orderby_dir;

        Item(std::shared_ptr<Col> col_, OrderByDir orderby_dir_) :
            expr(col_), col(std::move(col_)), orderby_dir(orderby_dir_) {}

        Item(std::shared_ptr<Expr> expr_, OrderByDir orderby_dir_) :
            expr(std::move(expr_)), col(std::dynamic_pointer_cast<Col>(expr)), orderby_dir(orderby_dir_) {}
    };

    // Preserve the original user-specified key order for lexicographic sorting.
    std::vector<std::shared_ptr<Item>> items;

    explicit OrderBy(std::vector<std::shared_ptr<Item>> items_) : items(std::move(items_)) {}
};

struct LimitClause : public TreeNode {
    int limit;
    int offset;

    LimitClause(int limit_, int offset_) : limit(limit_), offset(offset_) {}
};

struct InsertStmt : public TreeNode {
    std::string tab_name;
    std::vector<std::shared_ptr<Value>> vals;

    InsertStmt(std::string tab_name_, std::vector<std::shared_ptr<Value>> vals_) :
            tab_name(std::move(tab_name_)), vals(std::move(vals_)) {}
};

struct DeleteStmt : public TreeNode {
    std::string tab_name;
    std::vector<std::shared_ptr<BinaryExpr>> conds;

    DeleteStmt(std::string tab_name_, std::vector<std::shared_ptr<BinaryExpr>> conds_) :
            tab_name(std::move(tab_name_)), conds(std::move(conds_)) {}
};

struct UpdateStmt : public TreeNode {
    std::string tab_name;
    std::vector<std::shared_ptr<SetClause>> set_clauses;
    std::vector<std::shared_ptr<BinaryExpr>> conds;

    UpdateStmt(std::string tab_name_,
               std::vector<std::shared_ptr<SetClause>> set_clauses_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_) :
            tab_name(std::move(tab_name_)), set_clauses(std::move(set_clauses_)), conds(std::move(conds_)) {}
};

struct JoinExpr : public TreeNode {
    std::string left;
    std::string right;
    std::vector<std::shared_ptr<BinaryExpr>> conds;
    JoinType type;

    JoinExpr(std::string left_, std::string right_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_, JoinType type_) :
            left(std::move(left_)), right(std::move(right_)), conds(std::move(conds_)), type(type_) {}
};

struct SelectStmt : public TreeNode {
    std::vector<std::shared_ptr<SelectItem>> select_items;
    std::vector<std::string> tabs;
    std::vector<std::shared_ptr<BinaryExpr>> conds;
    std::vector<std::shared_ptr<JoinExpr>> jointree;

    
    bool has_sort;
    std::shared_ptr<OrderBy> order;
    
    bool has_group_by;
    std::shared_ptr<GroupBy> group_by;
    
    bool has_having;
    std::vector<std::shared_ptr<HavingCond>> having_conds;
    bool has_limit;
    std::shared_ptr<LimitClause> limit_clause;

    SelectStmt(std::vector<std::shared_ptr<SelectItem>> select_items_,
               std::vector<std::string> tabs_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_,
               std::shared_ptr<OrderBy> order_,
               std::shared_ptr<GroupBy> group_by_,
               std::vector<std::shared_ptr<HavingCond>> having_conds_,
               std::shared_ptr<LimitClause> limit_clause_) :
            select_items(std::move(select_items_)), tabs(std::move(tabs_)), conds(std::move(conds_)),
            order(std::move(order_)), group_by(std::move(group_by_)), having_conds(std::move(having_conds_)),
            limit_clause(std::move(limit_clause_)) {
                has_sort = (bool)order;
                has_group_by = (bool)group_by;
                has_having = !having_conds.empty();
                has_limit = (bool)limit_clause;
            }

    SelectStmt(std::vector<std::shared_ptr<SelectItem>> select_items_,
               std::vector<std::string> tabs_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_,
               std::vector<std::shared_ptr<JoinExpr>> jointree_,
               std::shared_ptr<OrderBy> order_,
               std::shared_ptr<GroupBy> group_by_,
               std::vector<std::shared_ptr<HavingCond>> having_conds_,
               std::shared_ptr<LimitClause> limit_clause_) :
            select_items(std::move(select_items_)), tabs(std::move(tabs_)), conds(std::move(conds_)),
            jointree(std::move(jointree_)),
            order(std::move(order_)), group_by(std::move(group_by_)), having_conds(std::move(having_conds_)),
            limit_clause(std::move(limit_clause_)) {
                has_sort = (bool)order;
                has_group_by = (bool)group_by;
                has_having = !having_conds.empty();
                has_limit = (bool)limit_clause;
            }
};

// set enable_nestloop
struct SetStmt : public TreeNode {
    SetKnobType set_knob_type_;
    bool bool_val_;

    SetStmt(SetKnobType &type, bool bool_value) : 
        set_knob_type_(type), bool_val_(bool_value) { }
};

// Semantic value
struct SemValue {
    int sv_int;
    float sv_float;
    std::string sv_str;
    bool sv_bool;
    OrderByDir sv_orderby_dir;
    std::vector<std::string> sv_strs;

    std::shared_ptr<TreeNode> sv_node;

    SvCompOp sv_comp_op;

    std::shared_ptr<TypeLen> sv_type_len;

    std::shared_ptr<Field> sv_field;
    std::vector<std::shared_ptr<Field>> sv_fields;

    std::shared_ptr<Expr> sv_expr;
    std::vector<std::shared_ptr<Expr>> sv_exprs;
    std::shared_ptr<SelectItem> sv_select_item;
    std::vector<std::shared_ptr<SelectItem>> sv_select_items;

    std::shared_ptr<Value> sv_val;
    std::vector<std::shared_ptr<Value>> sv_vals;

    std::shared_ptr<Col> sv_col;
    std::vector<std::shared_ptr<Col>> sv_cols;

    std::shared_ptr<SetClause> sv_set_clause;
    std::vector<std::shared_ptr<SetClause>> sv_set_clauses;

    std::shared_ptr<BinaryExpr> sv_cond;
    std::vector<std::shared_ptr<BinaryExpr>> sv_conds;

    std::shared_ptr<JoinExpr> sv_join_expr;
    std::vector<std::shared_ptr<JoinExpr>> sv_join_exprs;
    JoinType sv_join_type;

    std::shared_ptr<OrderBy::Item> sv_orderby_item;
    std::vector<std::shared_ptr<OrderBy::Item>> sv_orderby_items;
    std::shared_ptr<OrderBy> sv_orderby;
    std::shared_ptr<LimitClause> sv_limit_clause;
    
    // aggregation concerning
    std::shared_ptr<AggFunc> sv_agg_func;
    std::shared_ptr<GroupBy> sv_group_by;
    std::shared_ptr<HavingCond> sv_having_cond;
    std::vector<std::shared_ptr<HavingCond>> sv_having_conds;

    SetKnobType sv_setKnobType;
};

}

#define YYSTYPE ast::SemValue
