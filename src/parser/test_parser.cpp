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

int main() {
    std::vector<std::string> sqls = {
        "show tables;",
        "show index from tb;",
        "desc tb;",
        "create table tb (a int, b float, c char(4));",
        "drop table tb;",
        "create index tb(a);",
        "create index tb(a, b, c);",
        "drop index tb(a, b, c);",
        "drop index tb(b);",
        "insert into tb values (1, 3.14, 'pi');",
        "delete from tb where a = 1;",
        "update tb set a = 1, b = 2.2, c = 'xyz' where x = 2 and y < 1.1 and z > 'abc';",
        "select * from tb;",
        "select * from tb where x <> 2 and y >= 3. and z <= '123' and b < tb.a;",
        "select x.a, y.b from x, y where x.a = y.b and c = d;",
        "select x.a, y.b from x join y where x.a = y.b and c = d;",
        "exit;",
        "help;",
        "",
    };
    std::vector<std::string> aggSqls = {
        "select v1, count(*) as v2, count(v1) as v3, sum(v1) as v4, max(v1) as v5, min(v1) as v6 from t1;",
        "select id,MAX(score) as max_score,MIN(score) as min_score,SUM(score) as sum_score from grade group by id;",
        "select id,MAX(score) as max_score from grade group by id, course having v1 > 0;", // 需要手动检查
        "select id, MAX(score) as max_score from grade where MAX(score) > 90 group by id;" // 语法分析报错时抛出即可
    };
    std::vector<std::string> subquerySqls = {
        // 为了减少编写词法规则工作量，在分析阶段判断非法 sqls
        "select id from grade where score = (select MAX(score) from grade);",
        "select id from grade where score > (select score from grade);",
        "select id from grade where score < (select MAX(score) from grade);",
        "select id from grade where score = (select MIN(score) from grade where score > (select MAX(score) from grade));",
        // 多级嵌套
        "select id from grade where name in (select name from grade);",
        "select id from grade where score = (select MIN(score) from grade where score in (select MAX(score) from grade));"
    };
    std::vector<std::string> constValueSubquerySqls = {
        // 为了减少编写词法规则工作量，在分析阶段判断非法 sqls
        "select id from grade where score = (1);",
        "select id from grade where score > (4);",
        "select id from grade where score < (7, 8.0, '9');",
        "select id from grade where score >= (999.0);", // 多级嵌套
        "select id from grade where name in (1, 3.45, '4');"
    };
    std::vector<std::string> PerformanceSqls = {
        // 为了减少编写词法规则工作量，在分析阶段判断非法 sqls
        "load ../../src/test/performance_test/table_data/warehouse.csv into warehouse;",
        "load ../../src/test/performance_test/table_data/city.csv into city;",
        "load ../../src/test/performance_test/table_data/city.csv into orders;",
        "load ../my.csv into your;",
        "set output_file off",
        "set output_file on"
    };
    std::vector<std::string> DatetimeSqls = {
        // 为了减少编写词法规则工作量，在分析阶段判断非法 sqls
        "create table t(id int , time datetime);",
        "insert into t values(1, '2023-05-18 09:12:19');"
    };
    for (auto &sql: DatetimeSqls) {
        std::cout << sql << std::endl;
        YY_BUFFER_STATE buf = yy_scan_string(sql.c_str());
        assert(yyparse() == 0);
        if (ast::parse_tree != nullptr) {
            ast::TreePrinter::print(ast::parse_tree);
            yy_delete_buffer(buf);
            std::cout << std::endl;
        } else {
            std::cout << "exit/EOF" << std::endl;
        }
    }
    ast::parse_tree.reset();
    return 0;
}
