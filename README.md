# lsm-kv —— 从零实现的迷你 LSM-tree KV 存储引擎

考研复试项目第三弹。参考 [Tiny-LSM](https://github.com/Vanilla-Beauty/tiny-lsm) 的
lab 划分与 [LevelDB](https://github.com/google/leveldb) 的架构，代码全部自己实现。
纯 C++17 + Makefile，零第三方依赖。

## 架构（随阶段更新）

```
put(k,v) ─> WAL(阶段4) + MemTable(跳表)     ← 阶段 0
              │ 写满 flush
              ▼
           SST 文件(块化编码+稀疏索引)       ← 阶段 1
              │ 后台归并
              ▼
           多层 compaction                   ← 阶段 3
get(k)   ─> MemTable → 布隆过滤器 → 逐层 SST ← 阶段 2
```

## 构建

```bash
make            # 编译所有测试
make run        # 运行全部测试
```

## 进度

- [x] 阶段 0：跳表 MemTable（[笔记](docs/00-stage0-skiplist.md)）
- [ ] 阶段 1：SST 编码 + 稀疏索引 + flush
- [ ] 阶段 2：读路径 + 布隆过滤器
- [ ] 阶段 3：多层 SST + compaction
- [ ] 阶段 4：WAL + 崩溃恢复
- [ ] 阶段 5：scan 迭代器 + 压测报告 + 封版
