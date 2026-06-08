# StrixDB SQL 测试套件说明

## 概述

本目录包含 StrixDB 的用户层 SQL 端到端测试套件。测试通过启动 `rmdb` 服务端、使用 `rmdb_client` 发送 SQL、再检查输出来验证功能正确性。

## 快速开始

```bash
# 运行所有测试
./test/run_all_tests.sh

# 只运行指定套件
./test/run_all_tests.sh basic         # 基础查询
./test/run_all_tests.sh dml           # DML 操作
./test/run_all_tests.sh aggregate     # 聚合函数
./test/run_all_tests.sh unique        # 唯一索引
./test/run_all_tests.sh access_path   # 访问路径选择
```

## 目录结构

```
test/
├── run_all_tests.sh            # 统一测试入口（包含所有测试函数与框架代码）
├── test_all.sql                # 统一参考 SQL（按功能分节注释，可手动逐段执行）
├── TEST_SPEC.md                # 本文件
└── framework/
    ├── test_runner.sh          # 可复用的测试框架函数库
    └── data_generator.sh       # 随机测试数据生成器
```

## 测试框架 API

`run_all_tests.sh` 内置了所有框架函数，无需 source 外部文件。`framework/test_runner.sh` 作为独立的可复用函数库保留。

| 函数 | 说明 |
|------|------|
| `framework_init <db_name>` | 初始化环境，启动 server |
| `framework_cleanup` | 停止 server，清理临时文件 |
| `run_sql <sql>` | 执行 SQL 并捕获输出（打印到 stderr，返回 stdout） |
| `send_sql <sql>` | 执行 SQL，不捕获输出（用于 DDL/DML） |
| `check_contains <desc> <output> <pattern>` | 检查输出包含某模式 → PASS/FAIL |
| `check_not_contains <desc> <output> <pattern>` | 检查输出不包含某模式 → PASS/FAIL |
| `check_row_count <desc> <output> <n>` | 通过 "Total record(s): N" 检查行数 |
| `check_count <desc> <output> <pattern> <n>` | 检查模式出现次数 → PASS/FAIL |
| `check_not_contains_regex <desc> <output> <pattern>` | 正则检查不包含 → PASS/FAIL |
| `framework_summary` | 打印 PASS/FAIL 汇总，有失败则 exit 1 |

全局变量 `PASS` 和 `FAIL` 用于累计计数。

## 新增测试指南

### 步骤 0：确定 SQL 归属

- **纯 SQL 场景**（DDL/DML/查询）：写入 `test_all.sql`，按 `@@section <name>` / `@@endsection` 格式创建或扩展节。
- **需要进程编排的场景**（崩溃恢复、并发控制）：数据准备 SQL 写入 `test_all.sql` 独立节，由 `run_all_tests.sh` 的测试函数提取执行；多会话并发场景优先使用 `execute_interleaved_section` 读取同一分节。
- **题目要求完整比较 `output.txt` 的参考场景**：允许保留专用脚本，但脚本应只承担进程编排与期望文件比较，普通 SQL 与断言不再分散到额外脚本。

多会话分节可使用以下注释标记：

```sql
-- @session t1
-- @label readable assertion name
-- @expect contains expected text
-- @expect not_contains forbidden text
-- @expect row_count 1
-- @close t1
```

### 步骤 1：在 run_all_tests.sh 中添加测试函数

测试函数按命名约定 `test_<name>` 定义，例如：

```bash
test_my_feature() {
    framework_init "test_my_feature"

    send_sql "create table foo (id int, val int);"
    send_sql "insert into foo values (1, 100);"

    RESULT=$(run_sql "select id, val from foo where id = 1;")
    check_contains "basic select" "$RESULT" "100"

    send_sql "drop table foo;"
    framework_cleanup
    framework_summary
}
```

### 步骤 2：注册到 TEST_FUNCTIONS 数组

```bash
TEST_FUNCTIONS=(
    ...
    "test_my_feature:My Feature 描述"
)
```

### 步骤 3（可选）：在 test_all.sql 中添加参考 SQL

在 `test_all.sql` 末尾追加一个带注释的新节，便于手动交互测试。

## 数据生成器

`framework/data_generator.sh` 提供随机数据生成函数：

| 函数 | 说明 |
|------|------|
| `rand_int [min] [max]` | 随机整数 |
| `rand_float [min] [max]` | 随机浮点数 |
| `rand_str [len]` | 随机字符串 |
| `gen_data_inserts <table> <row_count> <col:type...>` | 批量生成 INSERT |
| `gen_composite_index_dataset <row_count>` | 生成复合索引测试数据集 |

用法示例：

```bash
source "$SCRIPT_DIR/../framework/test_runner.sh"
source "$SCRIPT_DIR/../framework/data_generator.sh"

framework_init "test_large"

send_sql "create table warehouse (w_id int, w_tax float, w_name char(16));"

gen_data_inserts "warehouse" 1000 "w_id:int" "w_tax:float" "w_name:string" | while read -r sql; do
    send_sql "$sql"
done
```

## 测试覆盖范围

### 单元测试 (C++ / GTest)

| 模块 | 测试数量 | 覆盖内容 |
|------|---------|---------|
| PredicateNormalizer | 3 | 重复去重、等价推断、矛盾检测 |
| IndexMatcher | 3 | 最长前缀、范围条件、覆盖索引 |
| PlannerAccessPath | 5 | 条件拆分、空结果标志、最佳索引、回退策略 |
| IndexScanRange | 5 | 四种比较操作符、空范围交集 |
| ValueComparison | 3 | int/float 等价、normalizer 数值去重、执行层比较 |
| DmlAccessPath | 4 | 等价常量+复合索引、矛盾条件、DELETE/UPDATE 路径 |
| DeleteViaIndexScan | 4 | DeleteExecutor 以 IndexScan 为子执行器的 batch 删除路径 |
| UpdateViaIndexScan | 6 | UpdateExecutor 以 IndexScan 为子执行器的 batch 更新路径 |
| TransactionTest | 15 | begin/commit/abort、Wait-Die(both paths)、锁升级、幂等重入、并发插入、崩溃恢复(INSERT/DELETE/UPDATE) |
| TransactionStateUnitTest | 10 | 隔离级别、空事务边界、活跃性判断、时间戳边界、SSI 读集合/谓词读/写集合、rw 依赖边去重、SSI 状态清理、冲突异常分类 |
| SortExecutorTest | 8 | 单键升序/降序、多键混合方向、空输入、单行、全相同键、字符串列、浮点列 |
| NestedLoopJoinExecutorTest | 8 | 内连接等值、交叉连接、半连接、空左侧/右侧、无匹配、多匹配、多条件 |
| SortMergeJoinExecutorTest | 8 | 内连接单键、残留条件、半连接、空侧、多右侧匹配、无匹配键、全匹配同键 |
| JoinPlannerTest | 10 | 等值→SortMerge、非等值→NestedLoop、等值+非等值拆分、SortPlan子节点、禁用SMJ降级、半连接保留、Portal转换、ORDER BY SortPlan、交叉连接、多键等值 |

### SQL 测试

| 套件 | 测试组数 | 覆盖内容 |
|------|---------|---------|
| basic | 8 组 | 无条件扫描、int/string/float 等值、多条件 AND、列对比 |
| dml | 10 组 | INSERT/SELECT/DELETE/UPDATE 完整 CRUD + 索引表 DML |
| aggregate | 5 组 | COUNT/SUM/AVG/MIN/MAX、GROUP BY、HAVING、选择规则 |
| unique | 6 组 | 默认唯一索引、建索引冲突、插入更新冲突 |
| access_path | 7 组 (25 项) | 见下表 |
| dml_index_scan | 6 组 | DELETE/UPDATE 通过 IndexScan 子执行器的 batch 路径 |
| cross_type | 7 组 | int/float 跨类型等值与范围谓词、含索引表跨类型 |

### 访问路径选择测试详情

| 组 | 测试内容 | 验证点 |
|----|---------|-------|
| Group 1: 单列索引等值 | 等值匹配、无匹配空集、非索引列过滤 | IndexScan 正确返回行 |
| Group 2: 复合索引前导列 | 前导列等值、前导列+第二列、非前导列回退、索引扫描+剩余过滤 | 前导列匹配规则 |
| Group 3: 等价推断 | a=b & b=5 推断 a=5、去重、矛盾检测、等价+范围、多列等价链 | 谓词规范化正确 |
| Group 4: 范围条件 | >, >=, <, <=, BETWEEN, 空交集 | 四种范围操作符、交集计算 |
| Group 5: 索引选择策略 | 最长前缀优先、范围条件优先、无索引回退 | `match_best_index` 策略 |
| Group 6: 大量数据 | 100 行前导列等值、精确匹配、范围扫描 | 大数据量下正确性 |
| Group 7: DML 访问路径 | DELETE 走索引、UPDATE 走索引、矛盾条件 DML、去重 DML | end-to-end DML 路径 |

| transaction | 8 组 | 显式 BEGIN/COMMIT、回滚 INSERT/UPDATE/DELETE、多 DML 混合回滚、崩溃恢复（已提交/未提交INSERT/未提交DELETE/未提交UPDATE）、索引表一致性 |
| sort | 4 组 | ORDER BY 单列 ASC/DESC、字符串列排序、无排序全扫描 |
| join | 6 组 | 内连接等值、半连接、带 WHERE 过滤连接、交叉连接、无匹配行 |
| join_sort | 2 组 | 连接结果按数值 DESC 排序、连接结果按字符串 ASC 排序 |
| mvcc_ssi_draft | 1 组 | 严格回归：`si/Deadlock`、`si/Non_Repeatable_Read_Lost_Update`、`si/UpdateTest`、`si/WriteWriteConflictDeleteInsertTest`、`ser/phantom_read_test_4`，按 `output.txt` 完整输出比对 |
| mvcc_ssi_coverage | 1 组 | SI/SER 失败清单同名场景、整数和字符串边界、错误路径、事务合法和非法状态转换、SSI 谓词读和危险结构 |
| mvcc_client_failure | 1 组 | 服务端不可达时客户端返回非零状态并输出连接失败诊断 |

### 事务测试框架扩展

`test_transaction` 测试节引入了两个框架辅助函数：

| 函数 | 说明 |
|------|------|
| `txn_send <stmt>...` | 将多条 SQL 经同一客户端连接发送（维持服务端 txn_id 不变），无输出捕获 |
| `txn_run <stmt>...`  | 同上，但捕获并返回所有响应输出 |

以及两个崩溃恢复辅助函数：

| 函数 | 说明 |
|------|------|
| `crash_server` | 发送 `crash` 命令终止服务器，等待端口释放 |
| `restart_server` | 在同一 `$DB_NAME` 上重启服务器并更新 `SERVER_PID` |

运行方式：

```bash
./test/run_all_tests.sh transaction   # 仅事务套件
./test/run_all_tests.sh mvcc_ssi_draft      # 仅 MVCC/SSI 题面参考场景
./test/run_all_tests.sh mvcc_ssi_coverage   # 仅 MVCC/SSI 覆盖矩阵
./test/run_all_tests.sh mvcc_client_failure # 仅客户端外部故障
```

MVCC/SSI 覆盖审查见 `docs/mvcc_ssi/test_coverage_review.md`。该文档记录正向路径、边界路径、错误路径、状态机、等价类、变异测试视角、冗余测试和未覆盖场景。

## 未覆盖场景

以下场景建议在后续迭代中补充：

1. **ORDER BY 利用索引排序** — 索引可避免额外排序，当前未覆盖
2. **NULL 值处理** — 当前不支持 NULL，后续加入后需测试
4. **跨类型比较** — int vs float 的极端值情况
5. **多客户端并发** — 等待-死亡策略和死锁检测需多进程客户端配合，当前框架不支持
6. **统计信息驱动的索引选择** — 当前基于启发式，后续引入直方图后需测试
7. **索引覆盖 SELECT 列** — 覆盖索引可避免 heap fetch，但当前框架无法观测内部行为
