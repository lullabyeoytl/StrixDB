%{
#include "ast.h"
#include "yacc.tab.h"
#include <iostream>
#include <memory>

int yylex(YYSTYPE *yylval, YYLTYPE *yylloc, yyscan_t yyscanner);

void yyerror(YYLTYPE *locp, std::shared_ptr<ast::TreeNode> *result, yyscan_t yyscanner, const char* s) {
    (void)result;
    (void)yyscanner;
    std::cerr << "Parser Error at line " << locp->first_line << " column " << locp->first_column << ": " << s << std::endl;
}

using namespace ast;

void append_join_clause(std::vector<std::shared_ptr<JoinExpr>> &join_exprs,
                        std::vector<std::shared_ptr<TableRef>> &right_refs,
                        std::shared_ptr<TableRef> right_ref,
                        std::vector<std::shared_ptr<BinaryExpr>> conds,
                        JoinType type) {
    join_exprs.push_back(std::make_shared<JoinExpr>(std::string(), right_ref->alias, std::move(conds), type));
    right_refs.push_back(std::move(right_ref));
}
%}

// request a pure (reentrant) parser
%define api.pure full
// enable location in error handler
%locations
// enable verbose syntax error message
%define parse.error verbose
// expose yyscan_t in generated header for reentrant lexer
%code requires {
    typedef void* yyscan_t;
}
// thread-safe: pass result pointer (yyparse+yyerror only) and scanner (all)
%parse-param {std::shared_ptr<ast::TreeNode> *result}
%param {yyscan_t yyscanner}

// keywords
%token SHOW TABLES CREATE TABLE DROP DESC INSERT INTO VALUES DELETE FROM ASC ORDER BY LIMIT OFFSET AS
WHERE UPDATE SET SELECT INT CHAR FLOAT DATETIME INDEX AND ON SEMI JOIN EXIT HELP TXN_BEGIN TXN_COMMIT TXN_ABORT TXN_ROLLBACK ORDER_BY ENABLE_NESTLOOP ENABLE_SORTMERGE ENABLE_HASHJOIN
%token COUNT SUM AVG MIN MAX GROUP HAVING EXPLAIN ANALYZE
%token TRANSACTION ISOLATION LEVEL SNAPSHOT SERIALIZABLE
// non-keywords
%token LEQ NEQ GEQ T_EOF

// type-specific tokens
%token <sv_str> IDENTIFIER VALUE_STRING
%token <sv_int> VALUE_INT
%token <sv_float> VALUE_FLOAT
%token <sv_bool> VALUE_BOOL

// specify types for non-terminal symbol
%type <sv_node> stmt dbStmt ddl dml txnStmt setStmt selectStmt explainAnalyzeTarget
%type <sv_field> field
%type <sv_fields> fieldList
%type <sv_type_len> type
%type <sv_comp_op> op
%type <sv_expr> expr
%type <sv_select_item> selectItem
%type <sv_select_items> selectItemList selector
%type <sv_agg_func> agg_func
%type <sv_val> value
%type <sv_vals> valueList
%type <sv_str> tbName colName
%type <sv_strs> colNameList
%type <sv_table_ref> tableRef
%type <sv_table_refs> tableRefList
%type <sv_join_exprs> joinClauseList
%type <sv_join_type> joinType
%type <sv_col> col
%type <sv_expr> order_expr
%type <sv_cols> colList
%type <sv_set_clause> setClause
%type <sv_set_clauses> setClauses
%type <sv_cond> condition
%type <sv_conds> whereClause optWhereClause
%type <sv_orderby> opt_order_clause
%type <sv_orderby_items> order_clause
%type <sv_orderby_item> order_item
%type <sv_orderby_dir> opt_asc_desc
%type <sv_limit_clause> opt_limit_clause
%type <sv_group_by> opt_group_clause
%type <sv_having_cond> having_cond
%type <sv_having_conds> havingCondList opt_having_clause
%type <sv_setKnobType> set_knob_type

%%
start:
        stmt ';'
    {
        *result = $1;
        YYACCEPT;
    }
    |   HELP
    {
        *result = std::make_shared<Help>();
        YYACCEPT;
    }
    |   EXIT
    {
        *result = nullptr;
        YYACCEPT;
    }
    |   T_EOF
    {
        *result = nullptr;
        YYACCEPT;
    }
    ;

stmt:
        EXPLAIN ANALYZE explainAnalyzeTarget
    {
        $$ = std::make_shared<ExplainAnalyzeStmt>($3);
    }
    |
        dbStmt
    |   ddl
    |   dml
    |   txnStmt
    |   setStmt
    ;

explainAnalyzeTarget:
        dml
    |   ddl
    |   dbStmt
    |   txnStmt
    |   setStmt
    ;

txnStmt:
        TXN_BEGIN
    {
        $$ = std::make_shared<TxnBegin>();
    }
    |   TXN_COMMIT
    {
        $$ = std::make_shared<TxnCommit>();
    }
    |   TXN_ABORT
    {
        $$ = std::make_shared<TxnAbort>();
    }
    | TXN_ROLLBACK
    {
        $$ = std::make_shared<TxnRollback>();
    }
    ;

dbStmt:
        SHOW TABLES
    {
        $$ = std::make_shared<ShowTables>();
    }
    |   SHOW INDEX FROM tbName
    {
        $$ = std::make_shared<ShowIndex>($4);
    }
    ;

setStmt:
        SET set_knob_type '=' VALUE_BOOL
    {
        $$ = std::make_shared<SetStmt>($2, $4);
    }
    |   SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION
    {
        $$ = std::make_shared<SetIsolationLevelStmt>(IsolationLevel::SNAPSHOT_ISOLATION);
    }
    |   SET TRANSACTION ISOLATION LEVEL SERIALIZABLE
    {
        $$ = std::make_shared<SetIsolationLevelStmt>(IsolationLevel::SERIALIZABLE);
    }
    ;

ddl:
        CREATE TABLE tbName '(' fieldList ')'
    {
        $$ = std::make_shared<CreateTable>($3, $5);
    }
    |   DROP TABLE tbName
    {
        $$ = std::make_shared<DropTable>($3);
    }
    |   DESC tbName
    {
        $$ = std::make_shared<DescTable>($2);
    }
    |   CREATE INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<CreateIndex>($3, $5);
    }
    |   DROP INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<DropIndex>($3, $5);
    }
    ;

dml:
        INSERT INTO tbName VALUES '(' valueList ')'
    {
        $$ = std::make_shared<InsertStmt>($3, $6);
    }
    |   DELETE FROM tbName optWhereClause
    {
        $$ = std::make_shared<DeleteStmt>($3, $4);
    }
    |   UPDATE tbName SET setClauses optWhereClause
    {
        $$ = std::make_shared<UpdateStmt>($2, $4, $5);
    }
    |   selectStmt
    ;

selectStmt:
        SELECT selector FROM tableRefList optWhereClause opt_group_clause opt_having_clause opt_order_clause opt_limit_clause
    {
        std::vector<std::string> tabs;
        tabs.reserve($4.size());
        for (const auto &ref : $4) {
            tabs.push_back(ref->name);
        }
        $$ = std::make_shared<SelectStmt>($2, std::move(tabs), $5, $8, $6, $7, $9,
                                          std::vector<std::shared_ptr<JoinExpr>>(), std::move($4));
    }
    |   SELECT selector FROM tableRef joinClauseList optWhereClause opt_group_clause opt_having_clause opt_order_clause opt_limit_clause
    {
        std::vector<std::shared_ptr<TableRef>> table_refs{$4};
        std::vector<std::string> tabs{$4->name};
        std::string current_left = $4->alias;
        auto right_refs = std::move($<sv_table_refs>5);
        for (size_t i = 0; i < $5.size(); ++i) {
            auto &join_expr = $5[i];
            const auto &right_ref = right_refs[i];
            join_expr->left = current_left;
            table_refs.push_back(right_ref);
            tabs.push_back(right_ref->name);
            current_left = join_expr->right;
        }
        $$ = std::make_shared<SelectStmt>($2, std::move(tabs), $6, $9, $7, $8, $10,
                                          std::move($5), std::move(table_refs));
    }
    ;

joinClauseList:
        JOIN tableRef ON whereClause
    {
        append_join_clause($$, $<sv_table_refs>$, $2, $4, INNER_JOIN);
    }
    |   joinType JOIN tableRef ON whereClause
    {
        append_join_clause($$, $<sv_table_refs>$, $3, $5, $1);
    }
    |   joinClauseList JOIN tableRef ON whereClause
    {
        $$ = std::move($1);
        $<sv_table_refs>$ = std::move($<sv_table_refs>1);
        append_join_clause($$, $<sv_table_refs>$, $3, $5, INNER_JOIN);
    }
    |   joinClauseList joinType JOIN tableRef ON whereClause
    {
        $$ = std::move($1);
        $<sv_table_refs>$ = std::move($<sv_table_refs>1);
        append_join_clause($$, $<sv_table_refs>$, $4, $6, $2);
    }
    ;

joinType:
        SEMI
    {
        $$ = SEMI_JOIN;
    }
    ;

fieldList:
        field
    {
        $$ = std::vector<std::shared_ptr<Field>>{$1};
    }
    |   fieldList ',' field
    {
        $$.push_back($3);
    }
    ;

colNameList:
        colName
    {
        $$ = std::vector<std::string>{$1};
    }
    | colNameList ',' colName
    {
        $$.push_back($3);
    }
    ;

field:
        colName type
    {
        $$ = std::make_shared<ColDef>($1, $2);
    }
    ;

type:
        INT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_INT, sizeof(int));
    }
    |   CHAR '(' VALUE_INT ')'
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_STRING, $3);
    }
    |   FLOAT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_FLOAT, sizeof(float));
    }
    |   DATETIME
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_DATETIME, kDatetimeLen);
    }
    ;

valueList:
        value
    {
        $$ = std::vector<std::shared_ptr<ast::Value>>{$1};
    }
    |   valueList ',' value
    {
        $$.push_back($3);
    }
    ;

value:
        VALUE_INT
    {
        $$ = std::make_shared<IntLit>($1);
    }
    |   VALUE_FLOAT
    {
        $$ = std::make_shared<FloatLit>($1);
    }
    |   VALUE_STRING
    {
        $$ = std::make_shared<StringLit>($1);
    }
    |   VALUE_BOOL
    {
        $$ = std::make_shared<BoolLit>($1);
    }
    ;

condition:
        col op expr
    {
        $$ = std::make_shared<BinaryExpr>($1, $2, $3);
    }
    ;

optWhereClause:
        /* epsilon */ { /* ignore*/ }
    |   WHERE whereClause
    {
        $$ = $2;
    }
    ;

whereClause:
        condition 
    {
        $$ = std::vector<std::shared_ptr<BinaryExpr>>{$1};
    }
    |   whereClause AND condition
    {
        $$.push_back($3);
    }
    ;

col:
        tbName '.' colName
    {
        $$ = std::make_shared<Col>($1, $3);
    }
    |   colName
    {
        $$ = std::make_shared<Col>("", $1);
    }
    ;

colList:
        col
    {
        $$ = std::vector<std::shared_ptr<Col>>{$1};
    }
    |   colList ',' col
    {
        $$.push_back($3);
    }
    ;

op:
        '='
    {
        $$ = SV_OP_EQ;
    }
    |   '<'
    {
        $$ = SV_OP_LT;
    }
    |   '>'
    {
        $$ = SV_OP_GT;
    }
    |   NEQ
    {
        $$ = SV_OP_NE;
    }
    |   LEQ
    {
        $$ = SV_OP_LE;
    }
    |   GEQ
    {
        $$ = SV_OP_GE;
    }
    ;

expr:
        value
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    |   col
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    |   agg_func
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    ;

agg_func:
        COUNT '(' '*' ')'
    {
        $$ = std::make_shared<AggFunc>(AGG_COUNT, true, nullptr);
    }
    |   COUNT '(' col ')'
    {
        $$ = std::make_shared<AggFunc>(AGG_COUNT, false, $3);
    }
    |   SUM '(' col ')'
    {
        $$ = std::make_shared<AggFunc>(AGG_SUM, false, $3);
    }
    |   AVG '(' col ')'
    {
        $$ = std::make_shared<AggFunc>(AGG_AVG, false, $3);
    }
    |   MIN '(' col ')'
    {
        $$ = std::make_shared<AggFunc>(AGG_MIN, false, $3);
    }
    |   MAX '(' col ')'
    {
        $$ = std::make_shared<AggFunc>(AGG_MAX, false, $3);
    }
    |   SUM '(' '*' ')'
    {
        yyerror(&@1, result, yyscanner, "SUM(*) is not supported");
        YYERROR;
    }
    |   AVG '(' '*' ')'
    {
        yyerror(&@1, result, yyscanner, "AVG(*) is not supported");
        YYERROR;
    }
    |   MIN '(' '*' ')'
    {
        yyerror(&@1, result, yyscanner, "MIN(*) is not supported");
        YYERROR;
    }
    |   MAX '(' '*' ')'
    {
        yyerror(&@1, result, yyscanner, "MAX(*) is not supported");
        YYERROR;
    }
    ;

setClauses:
        setClause
    {
        $$ = std::vector<std::shared_ptr<ast::SetClause>>{$1};
    }
    |   setClauses ',' setClause
    {
        $$.push_back($3);
    }
    ;

setClause:
        colName '=' value
    {
        $$ = std::make_shared<ast::SetClause>($1, $3);
    }
    ;

selector:
        '*'
    {
        $$ = {};
    }
    |   selectItemList
    ;

selectItem:
        expr
    {
        $$ = std::make_shared<SelectItem>($1);
    }
    |   agg_func AS colName
    {
        $$ = std::make_shared<SelectItem>(std::static_pointer_cast<Expr>($1), $3);
    }
    ;

selectItemList:
        selectItem
    {
        $$ = std::vector<std::shared_ptr<SelectItem>>{$1};
    }
    |   selectItemList ',' selectItem
    {
        $$.push_back($3);
    }
    ;

opt_group_clause:
        GROUP BY colList
    {
        $$ = std::make_shared<GroupBy>($3);
    }
    |   /* epsilon */
    {
        $$ = nullptr;
    }
    ;

opt_having_clause:
        HAVING havingCondList
    {
        $$ = $2;
    }
    |   /* epsilon */
    {
        $$ = {};
    }
    ;

havingCondList:
        having_cond
    {
        $$ = std::vector<std::shared_ptr<ast::HavingCond>>{$1};
    }
    |   havingCondList AND having_cond
    {
        $$.push_back($3);
    }
    ;

having_cond:
        agg_func op value
    {
        $$ = std::make_shared<ast::HavingCond>(true, $1, nullptr, $2, std::static_pointer_cast<Expr>($3));
    }
    |   col op value
    {
        $$ = std::make_shared<ast::HavingCond>(false, nullptr, $1, $2, std::static_pointer_cast<Expr>($3));
    }
    ;

tableRefList:
        tableRef
    {
        $$ = std::vector<std::shared_ptr<TableRef>>{$1};
    }
    |   tableRefList ',' tableRef
    {
        $$.push_back($3);
    }
    ;

tableRef:
        tbName
    {
        $$ = std::make_shared<TableRef>($1);
    }
    |   tbName IDENTIFIER
    {
        $$ = std::make_shared<TableRef>($1, $2);
    }
    |   tbName AS IDENTIFIER
    {
        $$ = std::make_shared<TableRef>($1, $3);
    }
    ;

opt_order_clause:
    ORDER BY order_clause      
    { 
        $$ = std::make_shared<OrderBy>($3); 
    }
    |   /* epsilon */ { $$ = nullptr; }
    ;

opt_limit_clause:
    LIMIT VALUE_INT
    {
        $$ = std::make_shared<LimitClause>($2, 0);
    }
    |   LIMIT VALUE_INT OFFSET VALUE_INT
    {
        $$ = std::make_shared<LimitClause>($2, $4);
    }
    |   /* epsilon */ { $$ = nullptr; }
    ;

order_clause:
      order_item
    { 
        $$ = std::vector<std::shared_ptr<OrderBy::Item>>{$1};
    }
    |   order_clause ',' order_item
    {
        $1.push_back($3);
        $$ = std::move($1);
    }
    ;

order_item:
      order_expr opt_asc_desc
    {
        $$ = std::make_shared<OrderBy::Item>($1, $2);
    }
    ;

order_expr:
      col
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    |   agg_func
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    ;

opt_asc_desc:
    ASC          { $$ = OrderBy_ASC;     }
    |  DESC      { $$ = OrderBy_DESC;    }
    |       { $$ = OrderBy_DEFAULT; }
    ;    

set_knob_type:
    ENABLE_NESTLOOP { $$ = EnableNestLoop; }
    |   ENABLE_SORTMERGE { $$ = EnableSortMerge; }
    |   ENABLE_HASHJOIN { $$ = EnableHashJoin; }
    ;

tbName: IDENTIFIER;

colName: IDENTIFIER;
%%
