# 全文索引优化与回归验证

工作分支：`gjc`。起点为参赛仓库 `main` 的
`70c8949787dae78a95c2a532c38adbbe4d23b4d9`，保留队友已有的 AI 函数修复。
修改范围为全文分词、停用词查询和 DaaT 合并路径，没有修改赛事 SQL、数据生成器、
评分脚本或 baseline JSON。

## 实现

- 每次分词独占的词频表移除 bucket 锁；只读停用词表移除读锁。
  重复词的查找和递增合并为一次探测，保留原有首次计数及最后出现的等价词拼写。
- 内置停用词增加只读 ASCII 查询表。仅 UTF8MB4 的纯英文字母走此路径；
  重音字符、其他字符及其他字符集仍走原有转换和 collation 比较。
- IK 字符分类只解码一次 Unicode code point，保留分类优先级；
  各 processor 复用已取得的字符信息。词典范围比较复用相同的字节比较语义。
- DaaT 对连续输出批量设置 datum 和 evaluated flags；非 batch 表达式保留旧路径。
  二进制文档 ID 使用同语义字节比较，固定数组不再逐次调用虚拟 `at()`。
  仅针对同一 DOUBLE 评分槽上的纯 BOOL 转换省去通用逐行表达式求值；
  阈值、复合表达式和标量路径仍使用原有 evaluator。

## 正确性

`fts_hot_path_consistency` 位于现有 `fts_index` mysqltest suite。
其预期结果由未经本次优化的原始二进制生成，覆盖重复词、大小写、重音、
补充平面字符、IK 模式、1024 行批处理、评分排序、LIMIT/OFFSET、BOOL 结果复用、
阈值/零分/OR 谓词、prepared statement 参数切换及 UPDATE/DELETE。

`run_fts_sql_contracts.py` 使用真实 SQL 引擎，依赖 PyMySQL。
它创建并在结束时删除随机命名的独立数据库；需要建库和删库权限，
请使用专用开发实例。密码从 `MYSQL_PWD` 读取，不写入报告。

先运行原始二进制并记录 oracle，再以同样配置运行优化二进制进行比较：

```sh
python3 unittest/sql/engine/expr/run_fts_sql_contracts.py \
  --host 127.0.0.1 --port 2883 --user root \
  --snapshot /tmp/fts-before.json

# 将专用实例切换到优化二进制，然后运行：
python3 unittest/sql/engine/expr/run_fts_sql_contracts.py \
  --host 127.0.0.1 --port 2883 --user root \
  --snapshot /tmp/fts-after.json --compare /tmp/fts-before.json --concurrency 8
```

本次验证：835 个结果/错误码检查与原始引擎一致，其中 18 个为原始引擎也返回的
错误码；额外 2048 次 TOKENIZE 查询在 8 个连接上通过。
原有 `tokenize_function`、`cn_word`、`parser_properties` 和新增
`fts_hot_path_consistency` mysqltest 均通过。
上述测试不能代替全部 SQL 回归或官方隐藏用例。

## 2026-11-05 本机性能实验

Apple M5 / 24 GB / macOS 26.5 / arm64，Clang 17，SDK 15.4，
`RelWithDebInfo`、unity build。完整编译成功。
两次测量轮流启动原始和优化二进制，使用同一独立数据目录、相同配置、4 个 CPU，
测量期间没有并行编译或 CPU profiling。启动时使用的 `memory_limit=6G` 已废弃，
实际内存预算由引擎自动计算；两次运行保持相同，不把它宣称为 6 GB 限制。

使用未经修改的 `tools/benchmark/fts_large_bench.sh`：
`ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`。
这些是同机耗时对比，**不是平台得分**。构建耗时仅测量一次，有波动；
TOKENIZE 和查询为各 3 组采样的平均值。

| 指标 | 原始版本 | 优化版本 | 耗时下降 |
| --- | ---: | ---: | ---: |
| IK 全字段建索引（秒） | 2.355 | 2.037 | 13.50% |
| IK content 建索引（秒） | 1.739 | 1.635 | 5.98% |
| BENG 建索引（秒） | 0.934 | 0.932 | 0.21% |
| TOKENIZE IK（毫秒） | 0.0743 | 0.0682 | 8.21% |
| TOKENIZE BENG（毫秒） | 0.0441 | 0.0413 | 6.35% |
| 中文查询（毫秒） | 0.8843 | 0.7109 | 19.61% |
| 英文查询（毫秒） | 1.2945 | 1.0295 | 20.47% |
| 中英混合查询（毫秒） | 0.9966 | 0.7864 | 21.09% |
| LIMIT 查询（毫秒） | 1.0866 | 1.0352 | 4.73% |

四项查询命中数在两次运行中均为 `8001 / 11000 / 7332 / 20`。
三类指标内部平均后等权平均，耗时下降约 **10.11%**；建索引、分词、查询
三类分别为 6.57%、7.28%、16.48%。不要把不同机器的绝对毫秒数直接比较。

## 平台结果与下一步

用户提供的第一次平台评测针对原始 `main`，执行成功但得分 0，
`fts_mean_improvement=-0.13572435493600427`。
平台基准与仓库内旧 baseline JSON 表现不一致，因此不用旧 JSON 推断实际分数。
本次优化的 Linux 平台结果尚待提测；未验证满分。

请在平台填写远程已推送的 `gjc` 分支和对应完整 commit SHA，返回完整 metrics
及执行日志后继续优化。后续重点是建索引中的排序/行生成，以及查询 LIMIT 路径。
规则以[官方全文索引调优题面](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/za7cfo5cazcrihhq)为准。
