# lsm-kv —— 从零实现的迷你 LSM-tree KV 存储引擎

考研复试项目第三弹。参考 [Tiny-LSM](https://github.com/Vanilla-Beauty/tiny-lsm) 的
lab 划分与 [LevelDB](https://github.com/google/leveldb) 的架构，代码全部自己实现。
纯 C++17 + Makefile，零第三方依赖。

**复习主入口：[REVIEW.md](REVIEW.md)**（复试前的三天复习计划 + 20 道预设问答）。

## 架构

```
put(k,v) ─> WAL(顺序日志) + MemTable(跳表)          阶段 0/4
              │ 写满 1MB flush
              ▼
           L0 SST(数据区+布隆+稀疏索引+footer)        阶段 1
              │ L0 个数 ≥4 触发 k 路归并
              ▼
           L1 SST(全量底级，墓碑物理删除)              阶段 3
get(k)   ─> MemTable → L0(新→旧) → L1                阶段 2
             每层先过布隆过滤器；墓碑截断下探
scan     ─> 同一套归并框架，返回存活记录               阶段 5
```

## 构建

```bash
make            # 编译全部测试
make run        # 跑全部测试（全绿才算通过）
make bench      # 基准测试
```

## 基准数据（WSL2 Ubuntu，gcc 15，10 万次/场景）

| 场景 | 吞吐 |
|---|---|
| 顺序 put（含 flush/compaction） | 18,898 ops/s |
| 随机 put | 9,292 ops/s |
| 命中 get | 47,431 ops/s |
| 未命中 get（布隆排除） | **7,870,321 ops/s** |
| 范围 scan（400 行/次） | 30 scans/s |
| put（fsync WAL） | 2,051 ops/s |

## 进度（全部完成，tag `v0.1`）

- [x] 阶段 0：跳表 MemTable（[笔记](docs/00-stage0-skiplist.md)）
- [x] 阶段 1：SST 编码 + 稀疏索引 + flush（[笔记](docs/01-stage1-sst.md)）
- [x] 阶段 2：读路径 + 布隆过滤器 + 引擎类（[笔记](docs/02-stage2-readpath.md)）
- [x] 阶段 3：多层 SST + compaction（[笔记](docs/03-stage3-compaction.md)）
- [x] 阶段 4：WAL + 崩溃恢复（[笔记](docs/04-stage4-wal.md)）
- [x] 阶段 5：scan + 压测 + 封版（[笔记](docs/05-stage5-scan-bench.md)）

## 测试覆盖

- 跳表：10 万随机操作与 std::map 对拍
- SST：落盘→重开→全量点查/迭代对拍（含墓碑）
- 引擎：2 万随机写→重开→全量对拍 + 5000 不存在 key 布隆验证
- WAL：fork 子进程被 kill -9 → 重放恢复 → 崩溃后继续写 → 再开验证
- scan：30000 写后 500 次随机范围查询对拍
