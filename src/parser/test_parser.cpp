/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */
#undef NDEBUG

#include <cassert>

#include "parser.h"

namespace {

bool parse_sql(const std::string &sql) {
    ast::parse_tree.reset();
    YY_BUFFER_STATE buf = yy_scan_string(sql.c_str());
    int rc = yyparse();
    yy_delete_buffer(buf);
    return rc == 0 && ast::parse_tree != nullptr;
}

void test_explain_analyze_parser() {
    assert(parse_sql("explain analyze select a, b from t where a > 1 and b < 10;"));
    auto explain = std::dynamic_pointer_cast<ast::ExplainAnalyzeStmt>(ast::parse_tree);
    assert(explain != nullptr);
    auto select = std::dynamic_pointer_cast<ast::SelectStmt>(explain->statement);
    assert(select != nullptr);
    assert(select->table_refs.size() == 1);
    assert(select->table_refs[0]->name == "t");
    assert(select->table_refs[0]->alias == "t");

    assert(parse_sql("explain analyze select * from customers c join orders o on c.customer_id = o.customer_id where o.total_amount > 1000;"));
    explain = std::dynamic_pointer_cast<ast::ExplainAnalyzeStmt>(ast::parse_tree);
    assert(explain != nullptr);
    select = std::dynamic_pointer_cast<ast::SelectStmt>(explain->statement);
    assert(select != nullptr);
    assert(select->table_refs.size() == 2);
    assert(select->table_refs[0]->name == "customers");
    assert(select->table_refs[0]->alias == "c");
    assert(select->table_refs[1]->name == "orders");
    assert(select->table_refs[1]->alias == "o");
    assert(select->jointree.size() == 1);
    assert(select->jointree[0]->left == "c");
    assert(select->jointree[0]->right == "o");
    assert(select->jointree[0]->type == INNER_JOIN);
    assert(select->jointree[0]->conds.size() == 1);

    assert(parse_sql("explain analyze select c.name, o.order_id from customers AS c join orders AS o on c.customer_id = o.customer_id;"));
    explain = std::dynamic_pointer_cast<ast::ExplainAnalyzeStmt>(ast::parse_tree);
    assert(explain != nullptr);
    select = std::dynamic_pointer_cast<ast::SelectStmt>(explain->statement);
    assert(select != nullptr);
    assert(select->table_refs.size() == 2);
    assert(select->table_refs[0]->alias == "c");
    assert(select->table_refs[1]->alias == "o");

    assert(!parse_sql("explain analyze insert into t values (1, 2);"));
    assert(!parse_sql("explain analyze select * from customers c join orders o;"));
}

}

int main() {
    std::vector<std::string> sqls = {
        "show tables;",
        "desc tb;",
        "create table tb (a int, b float, c char(4));",
        "drop table tb;",
        "create index tb(a);",
        "create index tb(a, b, c);",
        "create unique index tb(a);",
        "create unique index tb(a, b);",
        "create table tb (a int unique, b float);",
        "create table tb (a int, b int, unique(a, b));",
        "drop index tb(a, b, c);",
        "drop index tb(b);",
        "insert into tb values (1, 3.14, 'pi');",
        "delete from tb where a = 1;",
        "update tb set a = 1, b = 2.2, c = 'xyz' where x = 2 and y < 1.1 and z > 'abc';",
        "select * from tb;",
        "select * from tb where x <> 2 and y >= 3. and z <= '123' and b < tb.a;",
        "select x.a, y.b from x, y where x.a = y.b and c = d;",
        "select x.a, y.b from x join y on x.a = y.b where c = d;",
        "select count(*), sum(a), avg(b), min(c), max(c) from tb;",
        "select a, count(*) from tb group by a;",
        "select a, count(*) from tb group by a having count(*) > 1 and a = 2 order by a asc;",
        "exit;",
        "help;",
        "",
    };
    for (auto &sql : sqls) {
        std::cout << sql << std::endl;
        YY_BUFFER_STATE buf = yy_scan_string(sql.c_str());
        assert(yyparse() == 0);
        if (ast::parse_tree != nullptr) {
            if (sql == "create index tb(a);") {
                auto create_index = std::dynamic_pointer_cast<ast::CreateIndex>(ast::parse_tree);
                assert(create_index != nullptr);
                assert(create_index->unique);
            }
            ast::TreePrinter::print(ast::parse_tree);
            yy_delete_buffer(buf);
            std::cout << std::endl;
        } else {
            std::cout << "exit/EOF" << std::endl;
        }
    }
    test_explain_analyze_parser();
    ast::parse_tree.reset();
    return 0;
}
