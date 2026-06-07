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
#include <memory>
#include <vector>

#include "parser.h"

namespace {

auto parse_sql(const std::string &sql, yyscan_t yyscanner) -> std::shared_ptr<ast::TreeNode> {
    std::shared_ptr<ast::TreeNode> parse_tree;
    YY_BUFFER_STATE buf = yy_scan_string(sql.c_str(), yyscanner);
    int result = yyparse(&parse_tree, yyscanner);
    yy_delete_buffer(buf, yyscanner);
    if (result != 0) {
        return nullptr;
    }
    return parse_tree;
}

void assert_parse_ok(const std::string &sql, yyscan_t yyscanner) {
    auto parse_tree = parse_sql(sql, yyscanner);
    assert(parse_tree != nullptr);
}

void assert_parse_rejected(const std::string &sql, yyscan_t yyscanner) {
    auto parse_tree = parse_sql(sql, yyscanner);
    assert(parse_tree == nullptr);
}

void assert_union_derived_table_shape(const std::string &sql, yyscan_t yyscanner,
                                      const std::vector<std::string> &branch_tabs) {
    auto parse_tree = parse_sql(sql, yyscanner);
    auto select = std::dynamic_pointer_cast<ast::SelectStmt>(parse_tree);
    assert(select != nullptr);
    assert(select->table_refs.size() == 1);
    const auto &table_ref = select->table_refs.front();
    assert(table_ref->is_derived());
    assert(table_ref->alias == "u");
    auto union_query = std::dynamic_pointer_cast<ast::UnionQuery>(table_ref->query);
    assert(union_query != nullptr);
    assert(union_query->branches.size() == branch_tabs.size());
    for (size_t i = 0; i < branch_tabs.size(); ++i) {
        assert(union_query->branches[i]->tabs.size() == 1);
        assert(union_query->branches[i]->tabs.front() == branch_tabs[i]);
    }
}

void assert_union_derived_table_order_by(const std::string &sql, yyscan_t yyscanner,
                                         const std::string &col_name, ast::OrderByDir dir) {
    auto parse_tree = parse_sql(sql, yyscanner);
    auto select = std::dynamic_pointer_cast<ast::SelectStmt>(parse_tree);
    assert(select != nullptr);
    assert(select->has_sort);
    assert(select->order->items.size() == 1);
    auto col = std::dynamic_pointer_cast<ast::Col>(select->order->items.front()->expr);
    assert(col != nullptr);
    assert(col->col_name == col_name);
    assert(select->order->items.front()->orderby_dir == dir);
}

}  // namespace

int main() {
    std::vector<std::string> sqls = {
        "show tables;",
        "desc tb;",
        "create table tb (a int, b float, c char(4));",
        "drop table tb;",
        "create index tb(a);",
        "create index tb(a, b, c);",
        "create table tb (a int, b float);",
        "drop index tb(a, b, c);",
        "drop index tb(b);",
        "insert into tb values (1, 3.14, 'pi');",
        "delete from tb where a = 1;",
        "update tb set a = 1, b = 2.2, c = 'xyz' where x = 2 and y < 1.1 and z > 'abc';",
        "select * from tb;",
        "select * from tb where x <> 2 and y >= 3. and z <= '123' and b < tb.a;",
        "select x.a, y.b from x, y where x.a = y.b and c = d;",
        "select x.a, y.b from x join y on x.a = y.b and c = d;",
        "select count(*), sum(a), avg(b), min(c), max(c) from tb;",
        "select count(*) as cnt, sum(a) as total from tb;",
        "select a, count(*) from tb group by a;",
        "select a, count(*) from tb group by a having count(*) > 1 and a = 2 order by a asc, c desc;",
        "set transaction isolation level snapshot isolation;",
        "set transaction isolation level serializable;",
        "create static_checkpoint;",
        "exit;",
        "help;",
        "",
    };
    yyscan_t yyscanner;
    yylex_init(&yyscanner);
    for (auto &sql : sqls) {
        std::cout << sql << std::endl;
        auto parse_tree = parse_sql(sql, yyscanner);
        if (parse_tree != nullptr) {
            ast::TreePrinter::print(parse_tree);
            std::cout << std::endl;
        } else {
            std::cout << "exit/EOF" << std::endl;
        }
    }
    assert_parse_ok("set transaction isolation level snapshot isolation;", yyscanner);
    assert_parse_ok("set transaction isolation level serializable;", yyscanner);
    assert_parse_ok("create static_checkpoint;", yyscanner);
    assert_parse_rejected("create static_checkoint;", yyscanner);
    assert_parse_rejected("create unique index tb(a);", yyscanner);
    assert_parse_rejected("create table tb (a int unique, b float);", yyscanner);
    assert_parse_rejected("create table tb (a int, b int, unique(a, b));", yyscanner);
    assert_parse_rejected("set transaction isolation level unknown;", yyscanner);
    assert_parse_rejected("set isolation level snapshot;", yyscanner);
    assert_union_derived_table_shape("select * from (select * from t1 union select * from t2) as u order by c1 asc;", yyscanner, {"t1", "t2"});
    assert_union_derived_table_order_by("select * from (select * from t1 union select * from t2) as u order by c1 asc;", yyscanner, "c1", ast::OrderBy_ASC);
    assert_union_derived_table_shape("select * from (select * from t1 union select * from t2 union select * from t3) as u;", yyscanner, {"t1", "t2", "t3"});
    assert_parse_rejected("select * from (select * from t1 union) as u;", yyscanner);
    assert_parse_rejected("select * from (select * from t1 union select * from t2);", yyscanner);
    assert_parse_rejected("select * from (select * from t1 union select * from t2) as u, t3;", yyscanner);
    yylex_destroy(yyscanner);
    return 0;
}
