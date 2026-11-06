# 全文索引优化与回归验证

工作分支：`gjc`。起点为参赛仓库 `main` 的
`70c8949787dae78a95c2a532c38adbbe4d23b4d9`，保留队友已有的 AI 函数修复。
修改范围为全文分词、停用词查询、建索引批处理及检索执行路径，没有修改赛事 SQL、数据生成器、
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
`fts_hot_path_consistency`、`fts_bulk_build_consistency`、`fts_tokenize_reuse`
共 6 组 mysqltest 均通过。
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

## 2026-11-06 第二轮优化（待平台验证）

对应题面分词热路径、索引构建流程、解析器/内存复用及全文查询四个方向：

- 建索引的文档分词结果通过 batch 接口输出；每批只引用一个源文档，避免下一文档
  重用 arena 后使父算子引用失效。源存储扫描使用与其打开模式一致的 batch 接口，
  同时覆盖 MemTable 和 SSTable 路径。
- IK 和 BENG 在同一个 helper 内复用解析器元数据，文档对象和词项重新计算。
  TOKENIZE 在执行上下文内复用 helper 和词频表，parser、properties 或字符集变化
  会重建相关状态；不缓存输入文档的分词结果。
- 进程内共享内置 IK 只读词典 reader 和 cache pins，首次初始化互斥保护，完整初始化
  后才原子发布；在全局 KV cache 销毁之前释放词典引用。
- 对所有用途均为真值判断的自然语言 MATCH 查询，省去 BM25、倒排统计聚合等工作。
  数值投影、排序、非零阈值和 BOOLEAN MODE 保留原路径；prepared statement 的阈值
  参数通过计划约束校验。LIMIT 的隐式相关度排序仍计算原始得分。
  DaaT、TaaT 和稀疏 TaaT 均保留命中真值，长查询也使用有效的扫描范围。

新增回归覆盖导入后再建索引、超过一批的 1300 个不同词、空表、更新删除和重建索引、
TaaT 长查询、prepared 阈值切换，以及跨行 parser、配置、输出模式切换。
所有 mysqltest 预期结果均由原始二进制生成。
额外验证包括 8 个连接冷启动并发初始化（256 次 IK SQL），以及 MINOR FREEZE 后
对 4096 行表创建索引并使用该索引核对命中数与主键和。

同机配置和完整原样基准参数见上一节。11 月 6 日的原始版本与本轮版本对比如下；
构建耗时只有一次完整样本，不能据此判断几个百分点的差异具有统计意义：

| 指标 | 原始版本 | 本轮版本 | 耗时下降 |
| --- | ---: | ---: | ---: |
| IK 全字段建索引（秒） | 2.360 | 1.939 | 17.84% |
| IK content 建索引（秒） | 1.738 | 1.534 | 11.74% |
| BENG 建索引（秒） | 1.030 | 0.932 | 9.51% |
| TOKENIZE IK（毫秒） | 0.0751 | 0.0659 | 12.25% |
| TOKENIZE BENG（毫秒） | 0.0426 | 0.0404 | 5.16% |
| 中文查询（毫秒） | 0.8704 | 0.6019 | 30.85% |
| 英文查询（毫秒） | 1.2857 | 0.8931 | 30.54% |
| 中英混合查询（毫秒） | 0.9712 | 0.6787 | 30.12% |
| LIMIT 查询（毫秒） | 1.0705 | 1.0176 | 4.94% |

命中数保持 `8001 / 11000 / 7332 / 20`。构建、分词和查询三类平均耗时分别下降
13.03%、8.71%、24.11%，三类等权综合下降 **15.28%**。
这是相对原始 main 的本机对照，**不是官方基线的提升率或平台得分**，距离满分
要求仍有明显差距。未完成 Linux 整库 SQL 回归或官方隐藏用例验证。

## 平台结果与提测

用户提供的第一次平台评测针对原始 `main`，执行成功但得分 0，
`fts_mean_improvement=-0.13572435493600427`。
第一轮优化 `95ed256d0823e2ea747f23e36caf906145b1fc7a` 对应用户第二次返回的
评测结果（未提供平台 commit 字段，按提交顺序对应），仍为 0 分，
`fts_mean_improvement=-0.007609619281299838`，即较官方基线平均慢约 0.76%。
平台基准与仓库内旧 baseline JSON 表现不一致，因此不用旧 JSON 推断实际分数。

本轮需重新提交远程 `gjc` 分支的最新完整 commit SHA；`git rev-parse HEAD` 可以读取。
请返回完整 metrics 和执行日志，再决定下一轮瓶颈与优化。
满分条件是构建、分词、查询三类内部平均后等权综合耗时下降至少 50%，
不是相对本队上次提交提高 50%，也不是直接平均全部 9 项。
规则以[官方全文索引调优题面](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/za7cfo5cazcrihhq)为准。
