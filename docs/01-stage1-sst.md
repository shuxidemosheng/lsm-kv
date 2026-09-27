# 阶段 1：SST 编码 + 稀疏索引 + flush

日期：2026-09-28

## 文件格式（一次定死，避免后续改格式）

```
[data 区 ]  u8 tombstone | u32 klen | u32 vlen | key | value   （key 升序）
[index 区]  u32 klen | key | u64 offset                        （每 ~4KB 一条）
[footer ]  40 字节固定：magic LKVS | version | num_entries
           | index_offset | num_index | bloom_offset | bloom_len
```

- bloom 字段在阶段 1 就预留进 footer——格式一次定死，后续阶段不用迁移文件。
- SST **不可变**：这是 LSM 一切简单性的来源（读不需要加锁、不需要原地更新）。

## 关键设计

1. **稀疏索引**：每 ~4KB 数据一个点（首 key + 偏移），整个索引常驻内存。
   点查 = 二分索引定位块 → 顺序扫描 ~4KB。对比"每条记录都索引"（内存爆炸）
   和"无索引"（点查全文件扫），这是经典的折中。
2. **快排排除**：key < 文件最小 key 直接返回不存在，连二分都省。
3. **pread 随机读**：不依赖文件当前偏移，迭代器和点查可以并存。
4. **Finish 时 fsync**：SST 是"数据已持久"的凭证；只 write 不 fsync 的话
   页缓存里的数据在掉电时会丢（进程崩溃不会丢——这是阶段 4 要讲的区别）。

## 踩坑记录

1. **索引区长度算错**：第一次写成 `index_offset - index_offset = 0`。
   正确公式：`文件大小 - footer(40) - index_offset`。教训：偏移量语义
   （"这个字段指向哪里"）必须写成注释，写代码时对着注释核。
2. 忘记 `<sys/stat.h>`（fstat）。

## 测试

5000 条随机数据（含 10% 墓碑）经跳表 flush 成 SST → 重新打开（模拟重启）→
与 std::map oracle 全量点查对拍 + 迭代器逐条比对 + 1000 个不存在 key 全部正确返回。

## 实测数据

3667 个存活 key（含覆盖与墓碑）落盘后全部可查、迭代有序。
