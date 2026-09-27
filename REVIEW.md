# lsm-kv 复试复习总纲

> **阅读对象**：考研初试结束后的你。**目标**：2~3 天恢复到能现场讲清
> LSM-tree 每个组件的状态。与 `~/mydocker/REVIEW.md` 配套（网络/容器/存储三部曲）。

## 一、一句话定位

用 C++17 从零实现 LSM-tree 存储引擎：跳表 MemTable、块化 SST + 稀疏索引 +
布隆过滤器、k 路归并 compaction、WAL 崩溃恢复、范围扫描。≈ LevelDB 的教育版骨架。

## 二、代码地图

| 文件 | 内容 |
|---|---|
| `include/skiplist.h` + `src/skiplist.cpp` | 跳表（Put/Get/墓碑/有序遍历/Clear） |
| `include/bloom.h` | 布隆过滤器（double hashing、序列化） |
| `include/sst.h` + `src/sst.cpp` | SST 构建器 + 只读视图（footer/索引/布隆/迭代器） |
| `include/lsm_kv.h` + `src/lsm_kv.cpp` | 引擎：put/get/scan/flush/compaction/WAL |
| `test/` | 5 个测试 + 1 个 bench（全部与 oracle 对拍） |
| `docs/00~05-*.md` | 六篇阶段笔记 |

## 三、知识地图 ↔ 408

| 项目内容 | 408 考点 |
|---|---|
| 跳表（期望层数、查找路径） | 数据结构：随机化结构、有序表 |
| 布隆过滤器（误判率公式） | 数据结构：哈希、位图 |
| SST 稀疏索引 + 二分 | 数据结构：索引、查找 |
| MemTable→SST 顺序写、compaction 写放大 | OS：磁盘 IO 特性 |
| WAL、fsync、页缓存 vs 掉电 | OS：文件系统、可靠性 |
| OOM/墓碑/版本覆盖 | 数据管理综合 |

## 四、三天复习路线

- **D1**：README 架构图 → 跑 `make run && make bench`（五分钟复验）→ 六篇笔记；
- **D2**：按序精读源码：skiplist.cpp → bloom.h → sst.cpp（格式！能手画文件布局）→
  lsm_kv.cpp（Get 读路径 → FlushLocked → CompactLocked → Scan → WAL 重放）；
- **D3**：背自述与问答，演练现场演示（见第七节）。

## 五、3 分钟自述提纲

> 这个项目是我用 C++17 从零实现的 LSM-tree KV 存储引擎，对标 LevelDB 的核心机制。
> 写路径：数据先进内存跳表并落 WAL，写满后顺序刷成不可变的 SST 文件，
> L0 文件攒够 4 个触发 k 路归并压实进底层——这是 LSM 用顺序写替代随机写、
> 用写放大换读放大的核心权衡。读路径：先查 MemTable 再逐层查 SST，
> 每层先过布隆过滤器——我实测不存在 key 的查询达到每秒 787 万次，
> 比命中查询快两个数量级，这就是布隆对读放大的意义。删除用墓碑实现，
> 物理删除推迟到 compaction 的底层归并时才真正发生。持久性分两层：
> 进程崩溃靠 WAL 重放恢复——我写了 fork 加 kill -9 的真实崩溃测试验证
> 不丢数据；掉电安全靠 fsync 开关，实测代价是吞吐从 1.9 万掉到 2 千。
> 整个引擎的点查、归并、扫描共用同一套"最新版本获胜、墓碑截断"的版本规则。
> 和 LevelDB 的差距我也清楚：没有 block cache、compaction 在写线程同步执行、
> 没有 MANIFEST——这些是我知道的工程化方向。

## 六、预设问答（复试最可能追问）

1. **LSM-tree 为什么写快读慢？和 B+ 树怎么选？**
   写：内存吸收 + 顺序刷盘，无随机写；读：可能要查多层（读放大）。
   写多读少/追加型选 LSM（日志、消息、时序）；读多且要求低延迟选 B+ 树（MySQL）。
2. **跳表层数怎么定？期望层数？**
   每层以 1/4 概率升高，期望层数 = Σ(1/4)^i ≈ 1.33；查找 O(log₄n)。
   相比红黑树：无旋转、范围遍历沿 level0 链表即得、并发友好。
3. **布隆误判率公式？为什么会误判、会不会漏判？**
   p ≈ (1-e^(-kn/m))^k。哈希位全 1 才报"可能存在"——不同 key 可能哈希
   碰撞出相同位组合（误判），但存在过的 key 的位一定全 1（不漏判）。
   所以它只能做"否定过滤"。
4. **SST 点查的完整过程？**
   布隆排除 → key < 最小 key 排除 → 二分稀疏索引定位 4KB 块 → 块内顺序扫
   → key 相等返回 / 越过则不存在。
5. **compaction 时同 key 多版本怎么处理？墓碑什么时候能真删？**
   输入按新→旧排，同 key 取第一个出现（最新）并跳过其余；输出目标是底层
   （之下没有更老数据）时墓碑才能丢——否则会"复活"老版本。
6. **WAL 为什么能保证不丢？半条记录怎么办？**
   先写日志再改内存；崩溃后重放日志重建 MemTable。半条记录按长度校验
   识别并截断——恢复结果 = 已完整确认操作的前缀。进程崩溃只需 write 到
   页缓存；掉电需 fsync（引擎 sync_wal 开关，实测吞吐 1/9 的代价）。
7. **flush 时为什么要清空 WAL？**
   MemTable 数据已完整进入 SST（fsync 过），日志的使命完成；不清的话
   重放会把同样的数据灌两遍（幂等但浪费），且日志无限增长。
8. **读放大的量化？你怎么缓解的？**
   最坏要查 1(MemTable) + L0 个数 + 1(L1) 个文件。缓解：布隆（排除不存在）、
   compaction（减少 L0 个数）、稀疏索引（限制块内扫描范围）。
   进一步方向：block cache、前缀压缩。
9. **写放大的量化？**
   每条数据在 compaction 中被重写 log(n) 遍。实测：10 万条数据经历两次
   全量归并。方向：leveled compaction、WiscKey（kv 分离，本项目 config 里
   有占位）。
10. **和 LevelDB 的差距？**
    无 block cache、无 MANIFEST、compaction 同步执行、无迭代器快照、
    无并发控制、L1 全量归并写放大偏大。方向都对得上，机制讲得清。

## 七、五分钟复验（复试现场可演示）

```bash
cd ~/lsm-kv && make && make run    # 6 组测试全绿
make bench                          # 关键数字：miss get 787万/s、fsync 2千/s
# 现场演示崩溃恢复：test_wal 就是 fork + kill -9 的真实崩溃
```

## 八、档案

- **GitHub：https://github.com/shuxidemosheng/lsm-kv**（公开，按阶段提交 + tag v0.1）
- 归档：`D:\AIcoding\lsmkv-backup.tar.gz`
- 配套项目：`~/mydocker`（容器，GitHub 同名仓库）、`~/TinyWebServer`（网络）
