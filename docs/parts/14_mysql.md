# 第 17 章 · MySQL：命令与实现原理

▶ 对应程序：`ch17_mysql`

本章不需要安装 MySQL。原理部分全部用可运行的 C++ 模拟实现 —— 你能亲眼看到 B+ 树怎么长、MVCC 怎么判断可见性、Buffer Pool 怎么淘汰页。

## 1. SQL 的逻辑执行顺序

这是理解 SQL 的关键，也解释了两个高频困惑。

**书写顺序**：`SELECT … FROM … JOIN … ON … WHERE … GROUP BY … HAVING … ORDER BY … LIMIT`

**实际执行顺序**：

1. `FROM` / `JOIN` — 确定数据来源
2. `ON` — 连接条件过滤
3. `WHERE` — 行级过滤（此时还没分组，用不了聚合函数）
4. `GROUP BY` — 分组
5. **聚合函数** — `COUNT`/`SUM`/`AVG` 在这一步计算
6. `HAVING` — 组级过滤（可以用聚合函数）
7. `SELECT` — 选出列、计算表达式、赋别名
8. `DISTINCT` — 去重
9. `ORDER BY` — 排序（可以用别名，因为第 7 步已完成）
10. `LIMIT` — 取前 N 条

由此可解释：

- **为什么 `WHERE` 里不能用 `COUNT()`？** 因为 WHERE(3) 在聚合(5) 之前执行
- **为什么 `WHERE` 里不能用 `SELECT` 的别名，`ORDER BY` 却可以？** 因为 WHERE(3) 早于 SELECT(7)，ORDER BY(9) 晚于 SELECT(7)

## 2. 常用命令

### 2.1 建库建表

```sql
CREATE DATABASE shop
  DEFAULT CHARACTER SET utf8mb4
  COLLATE utf8mb4_0900_ai_ci;
```

**为什么必须 utf8mb4 而不是 utf8？** MySQL 的 `utf8` 是历史遗留的**残缺**实现，每字符最多 3 字节，存不了 emoji 和部分生僻汉字（它们需要 4 字节）。`utf8mb4` 才是真正的 UTF-8。老库用 `utf8` 存 emoji 会直接报错或截断。

```sql
CREATE TABLE orders (
    id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    user_id     BIGINT UNSIGNED NOT NULL,
    order_no    VARCHAR(32)     NOT NULL,
    amount      DECIMAL(12,2)   NOT NULL DEFAULT 0.00,
    status      TINYINT         NOT NULL DEFAULT 0,
    created_at  DATETIME(3)     NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (id),
    UNIQUE  KEY uk_order_no (order_no),
    KEY         idx_user_status_created (user_id, status, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='订单表';
```

每个决定都有理由：

| 决定 | 理由 |
|---|---|
| `BIGINT UNSIGNED AUTO_INCREMENT` 主键 | 顺序插入 → B+ 树不分裂（见 3.3） |
| `DECIMAL(12,2)` 存金额 | 绝不用 `FLOAT`/`DOUBLE`（第 13 章 1.5） |
| `NOT NULL` + `DEFAULT` | NULL 让索引统计和条件判断都变复杂 |
| `DATETIME(3)` | 带毫秒；`DATETIME` 不含时区，`TIMESTAMP` 含时区但只到 2038 年 |
| 联合索引列的顺序 | 见 5. 最左前缀 |

### 2.2 DELETE / TRUNCATE / DROP 的区别

| | 类型 | 可回滚 | 触发器 | 释放空间 | 自增值 |
|---|---|---|---|---|---|
| `DELETE` | DML，逐行删 | ✅（写 undo） | ✅ | ❌（只标记删除） | 不重置 |
| `TRUNCATE` | DDL，重建表空间 | ❌ | ❌ | ✅ | **重置为 1** |
| `DROP` | DDL | ❌ | ❌ | ✅ | — |

### 2.3 深分页的正确写法

```sql
-- 慢：要先扫描并丢弃前 100 万行
SELECT * FROM orders ORDER BY id LIMIT 1000000, 20;

-- 快：用上次的最大 id 做游标，直接定位（keyset 分页）
SELECT * FROM orders WHERE id > 1000000 ORDER BY id LIMIT 20;
```

### 2.4 取每组前 N 条（窗口函数，8.0+）

```sql
SELECT * FROM (
    SELECT *, ROW_NUMBER() OVER (
                 PARTITION BY user_id ORDER BY created_at DESC) AS rn
    FROM orders
) t WHERE rn <= 3;
```

### 2.5 生产环境的 ALTER 警告

大表 `ALTER` 会锁表或长时间占用资源。MySQL 5.6+ 支持 Online DDL，但并非所有操作都支持：

| ALGORITHM | 行为 |
|---|---|
| `INSTANT` | 秒级完成（8.0，加列在末尾） |
| `INPLACE` | 不拷表，但可能重建索引 |
| `COPY` | 拷全表，最慢，期间阻塞写 |

```sql
ALTER TABLE t ADD COLUMN c INT, ALGORITHM=INSTANT, LOCK=NONE;
```

大表变更推荐 `gh-ost` 或 `pt-online-schema-change`。

### 2.6 运维诊断速查

```sql
SHOW FULL PROCESSLIST;                  -- 当前连接与正在执行的语句
SHOW ENGINE INNODB STATUS;              -- InnoDB 全量状态（含最近一次死锁）
SELECT * FROM performance_schema.data_lock_waits;   -- 锁等待（8.0）
SELECT * FROM information_schema.innodb_trx ORDER BY trx_started;  -- 长事务

-- 慢查询日志
SET GLOBAL slow_query_log = ON;
SET GLOBAL long_query_time = 1;
-- 然后用 pt-query-digest 分析

ANALYZE TABLE orders;      -- 重新采样索引统计（执行计划不准时用）
```

## 3. 为什么索引用 B+ 树

### 3.1 候选方案的淘汰过程

| 方案 | 淘汰原因 |
|---|---|
| 哈希表 | O(1) 等值查找，但**不支持范围查询和排序**。`WHERE age > 20`、`ORDER BY age` 全部失效 |
| 二叉搜索树 / 红黑树 | 扇出只有 2 → 树高 O(log₂n)。100 万行 → 树高 20 → 20 次磁盘 IO |
| B 树 | 多路平衡，但**非叶子节点也存数据** → 每节点能放的索引项变少 → 扇出下降 → 树更高。且范围查询要中序遍历，来回跳节点 |
| **B+ 树** | ✅ 非叶子只存键 → 一页能放几百个键 ✅ 所有数据在叶子层且用双向链表连接 → 范围查询就是顺序扫链表 |

InnoDB 内部只在**自适应哈希索引（AHI）** 里用哈希表做等值加速。

### 3.2 关键容量计算

InnoDB 页大小 16 KB。非叶子节点每项 = 键(8B, BIGINT) + 页号(6B) = 14 B，**扇出 ≈ 16384 / 14 ≈ 1170**。

程序实测输出：

| 每行大小 | 每页行数 | 树高 2 | 树高 3 | 树高 4 |
|---|---|---|---|---|
| 100 B | 163 | 19 万 | **2 亿** | 2610 亿 |
| 200 B | 81 | 9 万 | 1 亿 | 1297 亿 |
| 500 B | 32 | 3 万 | 4380 万 | 512 亿 |
| 1000 B | 16 | 1 万 | 2190 万 | 256 亿 |

**结论：常规表（行 1 KB 以内）在树高 3 时就能存千万到上亿行。** 而根节点和大部分非叶子节点常驻 Buffer Pool，所以一次主键查找通常只有 0~1 次真实磁盘 IO。

对比红黑树：

| 行数 | B+ 树（扇出 1170） | 红黑树（扇出 2） |
|---|---|---|
| 1 万 | 2 层 | 14 层 |
| 100 万 | 3 层 | 20 层 |
| 1 亿 | **4 层** | **27 层** |

磁盘随机 IO 约 0.1 ms（SSD）到 10 ms（HDD）。27 次 IO 在 HDD 上就是 270 ms —— 一个查询走完就超时了。

**内存里红黑树很好（`std::map` 就用它），但磁盘索引必须降低树高。**

### 3.3 范围查询的实测差异

程序实测（order=8，5000 个键）：

- `find(3777)` — 访问 6 个节点（= 树高）
- `range(3000, 3050)` 返回 51 行 — 访问 19 个节点，其中 6 次用于定位起点，**其余是沿叶子链表顺序扫描**

B 树没有叶子链表，范围查询要不断回到父节点，全是随机 IO。这就是 B+ 树胜出的关键。

## 4. 聚簇索引与二级索引

**InnoDB 的表本身就是一棵 B+ 树**，这棵树叫聚簇索引，叶子节点直接存放完整的行数据。

```
聚簇索引（主键 id）                二级索引（KEY idx_name(name)）
┌───────────────────┐            ┌───────────────────┐
│   [10 | 50 | 90]  │  内部节点   │  [Bob | Tom]      │
└────┬──────┬───────┘            └────┬────────┬─────┘
┌────▼──┐ ┌─▼─────┐              ┌────▼──┐ ┌───▼───┐
│ id=10 │ │ id=50 │  叶子节点     │Alice  │ │Tom    │
│ 完整行│ │ 完整行│  存**数据**   │ id=50 │ │ id=10 │  存**主键值**
└───────┘ └───────┘              └───────┘ └───────┘
```

### 回表

```sql
SELECT * FROM t WHERE name = 'Tom';
```

1. 在 `idx_name` 上找到 `'Tom'` → 得到 `id=10`
2. 拿 `id=10` 再去聚簇索引查一遍 → 得到完整行

两棵树各走一遍，IO 翻倍。第二步就叫**回表**。

### 覆盖索引 —— 消除回表

```sql
SELECT id, name FROM t WHERE name = 'Tom';
```

需要的 `id` 和 `name` 都在 `idx_name` 里（name 是键，id 是值），**不用回表**。`EXPLAIN` 的 Extra 列会显示 `Using index`。

实用技巧：高频查询且只要少数几列时，把这几列加进索引：

```sql
KEY idx_name_age (name, age)    -- 现在 SELECT name, age 也不回表
```

代价：索引变大，写入变慢。典型的空间换时间。

### 为什么主键必须短且自增

1. **每个二级索引的叶子都存一份主键值。** 主键用 UUID（36 字节 varchar）而不是 BIGINT（8 字节），10 个二级索引就多占 280 字节/行
2. **自增主键 = 顺序插入** = 总是往最右边的页追加，页利用率高、几乎不分裂。随机主键（UUID）= 随机插入 = 频繁页分裂 + 页内碎片，实测写入性能差几倍
3. **没有显式主键时**，InnoDB 会：先找第一个 `NOT NULL` 的唯一索引；都没有就生成一个 6 字节的隐藏 `ROW_ID` —— 这个 ROW_ID 是**全局共享**的自增值，高并发插入会成为竞争点

**永远显式定义主键。** 业务需要 UUID 时：内部用自增 BIGINT 主键，UUID 作为带唯一索引的业务列；或用有序 UUID（UUIDv7 / 雪花 ID）。

## 5. 索引失效的 8 种场景

### 最左前缀原则

联合索引 `KEY idx_abc (a, b, c)` 的 B+ 树是按 `(a, b, c)` 的**字典序**排列的。就像电话簿按"姓, 名"排序：知道姓能快速定位，只知道名就只能全本翻。

| 条件 | 是否用到索引 |
|---|---|
| `WHERE a=1` | ✅ 用到 (a) |
| `WHERE a=1 AND b=2` | ✅ 用到 (a,b) |
| `WHERE a=1 AND b=2 AND c=3` | ✅ 全覆盖 |
| `WHERE a=1 AND c=3` | △ 只用到 (a)，c 无法用于定位 |
| `WHERE b=2` | ❌ 跳过了最左列 |
| `WHERE a=1 AND b>2 AND c=3` | △ a,b 用到；**b 是范围查询，c 无法再用于定位** |

**推论：范围列要放联合索引的最后。**

注意 `WHERE b=2 AND a=1` 是**可以**用到索引的 —— 优化器会自动调整条件顺序。"最左"指的是**索引列**的顺序，不是 SQL 里书写的顺序。

### 八种失效场景

**① 对索引列做运算或用函数**

```sql
✗ WHERE YEAR(created_at) = 2026
✓ WHERE created_at >= '2026-01-01' AND created_at < '2027-01-01'
```

索引里存的是原值，`YEAR(x)` 的结果没有索引。（8.0.13+ 支持函数索引：`ADD INDEX ((YEAR(created_at)))`）

**② 隐式类型转换 ← 最隐蔽的一个**

表里 `phone` 是 `VARCHAR`：

```sql
✗ WHERE phone = 13800138000      -- 数字！触发 CAST(phone AS SIGNED)
✓ WHERE phone = '13800138000'    -- 加引号
```

代码里传参类型写错就中招，而且**不报错，只是慢 1000 倍**。反过来 int 列写 `WHERE id = '99'` 不会失效（转换发生在常量侧）。

**这是全书隐蔽程度排名第一的坑**，比第 13 章任何一个 C++ 坑都难发现。

**③ 字符集/排序规则不一致的 JOIN** — 需要转换，索引失效。建库时统一字符集能避免。

**④ 前导模糊匹配**

```sql
✗ WHERE name LIKE '%tom'     ✗ WHERE name LIKE '%tom%'
✓ WHERE name LIKE 'tom%'
```

需要"包含"搜索：用全文索引（FULLTEXT）或 ES。

**⑤ OR 连接的条件中有非索引列** — 整个查询全表扫。给所有列建索引，或改写成 `UNION ALL`。

**⑥ `!=` / `<>` / `NOT IN` / `NOT EXISTS`** — 通常失效，但取决于选择性。

**⑦ `IS NOT NULL`** — InnoDB 的索引会存 NULL 值，所以 `IS NULL` 能用索引；`IS NOT NULL` 命中范围太大，常被放弃。

**⑧ 优化器主动放弃（这不是 bug）** — 当预估命中行数超过全表的 20~30% 时，优化器认为"随机回表 N 次"比"顺序全表扫"更贵。统计信息不准导致的误判用 `ANALYZE TABLE` 修正，或 `FORCE INDEX` 强制（慎用）。

### 索引选择性

选择性 = 不同值的数量 / 总行数。程序实测（100 万行）：

| 列 | 不同值 | 选择性 | 建索引？ |
|---|---|---|---|
| `id`（主键） | 1000000 | 1.00000 | 值得（唯一索引） |
| `order_no` | 999500 | 0.99950 | 值得（唯一索引） |
| `user_id` | 50000 | 0.05000 | 值得 |
| `city` | 300 | 0.00030 | 看查询模式 |
| `status` | 5 | 0.00001 | 单列不值得 |
| `is_deleted` | 2 | 0.00000 | 单列不值得 |

`status` / `is_deleted` 这类低选择性列**单独**建索引没意义（命中 20 万行，回表 20 万次比全表扫还慢），但放进联合索引的**非首列**很有用：

```sql
KEY idx_user_status (user_id, status)
```

先用高选择性的 `user_id` 把范围缩到几十行，再用 `status` 过滤。

## 6. 事务与隔离级别

### ACID 各自由什么机制保证

| | | 机制 |
|---|---|---|
| **A** | 原子性 | **undo log** —— 回滚时把数据改回去 |
| **C** | 一致性 | 上面三者 + 约束（外键/唯一/CHECK） |
| **I** | 隔离性 | **锁 + MVCC** |
| **D** | 持久性 | **redo log** —— 崩溃后重放已提交的修改 |

### 三类并发问题

| 问题 | 含义 |
|---|---|
| 脏读 | 读到了别的事务**还没提交**的修改。对方一回滚，你读到的就是从未存在的数据 |
| 不可重复读 | 同一事务内两次读**同一行**，值不一样（别的事务提交了 UPDATE） |
| 幻读 | 同一事务内两次执行**同一范围查询**，行数不一样（别的事务提交了 INSERT） |

注意区分：幻读针对"行的出现/消失"，不可重复读针对"已有行的值变化"。

### 四种隔离级别

| 级别 | 脏读 | 不可重复读 | 幻读 |
|---|---|---|---|
| READ UNCOMMITTED | 可能 | 可能 | 可能 |
| READ COMMITTED | 不会 | 可能 | 可能 |
| **REPEATABLE READ**（MySQL 默认） | 不会 | 不会 | 基本不会* |
| SERIALIZABLE | 不会 | 不会 | 不会 |

**\* 关于 RR 与幻读 —— 这是最容易讲错的地方：**

- **快照读**（普通 SELECT）：靠 MVCC，整个事务用同一个 ReadView，所以看不到别人新插入的行 → 无幻读
- **当前读**（`SELECT FOR UPDATE` / `UPDATE` / `DELETE`）：读最新版本，靠 next-key lock 锁住范围来防止插入 → 无幻读
- 所以 **InnoDB 的 RR 确实解决了幻读**，这与教科书上"RR 不解决幻读"的说法不同 —— 教科书讲的是 SQL 标准，InnoDB 的实现**比标准更强**
- 唯一残留的例外：先快照读，再在同一事务里对该行做当前读/更新，会"看到"自己之前看不到的行

**为什么 MySQL 默认 RR 而 PostgreSQL / Oracle / SQL Server 默认 RC？** 历史原因：早期的 binlog 只有 STATEMENT 格式，RC 下会导致主从数据不一致。现在用 ROW 格式已无此问题，很多大厂规范里明确要求改成 RC —— 因为 RC 的锁范围更小、间隙锁更少、死锁概率更低。

## 7. MVCC —— 让读不加锁

MVCC（Multi-Version Concurrency Control）让读操作去找"一个对我可见的历史版本"，而不是等写锁释放。于是**读写不互相阻塞**，这是 InnoDB 高并发的根本原因。

### 三个组成部分

1. 每行的隐藏列 `DB_TRX_ID`（谁改的）和 `DB_ROLL_PTR`（上个版本在哪）
2. undo log 里串起来的**版本链**
3. **ReadView** —— 事务开始快照读时拍下的"当时谁在运行"的快照

```
当前行 (trx_id=30) ──roll_ptr──► undo(trx_id=20) ──► undo(trx_id=10)
name='v3'                        name='v2'            name='v1'
```

### 可见性判断算法

ReadView 有 4 个字段：`m_ids`（创建时仍活跃的事务 id 集合）、`min_trx_id`、`max_trx_id`、`creator_trx_id`。

对每个版本（trx_id 记为 T）从新到旧依次判断：

1. `T == creator_trx_id` → **可见**（自己改的当然能看见）
2. `T < min_trx_id` → **可见**（在我拍快照前就已提交）
3. `T >= max_trx_id` → **不可见**（我拍快照之后才开启的事务）
4. `min_trx_id <= T < max_trx_id`：
   - `T ∈ m_ids` → **不可见**（拍快照时它还没提交）
   - `T ∉ m_ids` → **可见**（拍快照时它已提交）

不可见就顺 `roll_ptr` 往老版本走，直到找到可见的或链走完。

### RC 与 RR 的唯一区别

**就在什么时候创建 ReadView：**

- **RC**：每条 SELECT 语句都新建一个 → 能看到别人新提交的 → 不可重复读
- **RR**：事务内第一条 SELECT 建一个，之后一直复用 → 整个事务视图一致

### 程序实测

场景：事务 1 写入并提交；事务 2 开启（不提交）；事务 3 修改并提交；事务 4 修改但不提交。

```
【事务 2 做快照读（RR）】
    ReadView{creator=2, min=3, max=3, active={}}
      版本 1: trx_id=4 value="v3-事务4未提交"  -> 不可见（快照之后才开启的事务）
      版本 2: trx_id=3 value="v2-被事务3修改"  -> 不可见（快照之后才开启的事务）
      版本 3: trx_id=1 value="v1-初始值"       -> 可见（早于快照且已提交）
    结果: v1-初始值

【新事务做快照读（RC）】
    ReadView{creator=5, min=2, max=6, active={2,4}}
      版本 1: trx_id=4 value="v3-事务4未提交"  -> 不可见（快照时该事务仍未提交）
      版本 2: trx_id=3 value="v2-被事务3修改"  -> 可见（快照时该事务已提交）
    结果: v2-被事务3修改

【事务 4 读自己的未提交修改】
      版本 1: trx_id=4 value="v3-事务4未提交"  -> 可见（自己修改的）
    结果: v3-事务4未提交
```

### MVCC 的代价：长事务

只要还有事务的 ReadView 可能需要某个老版本，undo log 就**不能清理**。一个开着不提交的长事务（比如忘了 commit 的会话）会导致 undo 一直累积 —— 这是生产环境 ibdata 暴涨的经典原因。

排查：

```sql
SELECT trx_id, trx_started,
       TIMESTAMPDIFF(SECOND, trx_started, NOW()) AS duration_s,
       trx_rows_modified, trx_query
FROM information_schema.innodb_trx ORDER BY trx_started;
```

预防：设 `innodb_max_undo_log_size` + 开 `innodb_undo_log_truncate`；应用侧设事务超时，**别把事务跨越网络调用或用户交互**；监控长事务并告警。

## 8. 锁

**InnoDB 的锁加在索引上，不是加在行上** —— 这是理解所有锁问题的前提。

**推论**：如果 `WHERE` 条件用不到索引，就只能锁住扫过的**所有**记录，效果近似锁表。所以索引失效不仅慢，还会放大锁冲突。

### 三种行级锁

| 锁 | 含义 |
|---|---|
| Record Lock | 锁住索引上的一条具体记录 |
| Gap Lock | 锁住两条记录**之间**的空隙，阻止插入 → 防幻读 |
| **Next-Key Lock** | Record + 前面的 Gap，即**左开右闭** `(前一条, 本条]`。**RR 下的默认加锁方式** |

例：表里有 `id = 5, 10, 15, 20`，间隙为 `(-∞,5) (5,10) (10,15) (15,20) (20,+∞)`

| SQL | 加锁范围 |
|---|---|
| `WHERE id = 10 FOR UPDATE` | 唯一索引且记录存在 → 退化成 Record Lock，只锁这一行 |
| `WHERE id = 12 FOR UPDATE`（不存在） | **锁住间隙 (10,15)** —— 别人插 11/12/13/14 全部阻塞 |
| `WHERE id > 10 AND id <= 18 FOR UPDATE` | 锁 `(10,15]` 和 `(15,20]` —— **右边界扩到了 20！** |

"查不存在的行也会加锁"是很多插入死锁的源头。

### 死锁

**成因 1：加锁顺序相反**（与第 13 章 5.3 同源）

```
事务A: UPDATE WHERE id=1;  然后 WHERE id=2;
事务B: UPDATE WHERE id=2;  然后 WHERE id=1;
```

对策：让所有事务按**固定顺序**（比如 id 升序）访问行。批量更新前先 `ORDER BY id`。

**成因 2：唯一索引冲突 + 间隙锁** — 两个事务同时 INSERT 同一唯一键，先来的持有 X 锁，后来的检测到重复要加 S 锁等待，此时先来的回滚 → 死锁。对策：用 `INSERT … ON DUPLICATE KEY UPDATE`。

**排查步骤**：

1. `SHOW ENGINE INNODB STATUS;` 看 `LATEST DETECTED DEADLOCK` 段 —— 它会明确列出两个事务各自持有什么锁、在等什么锁、执行的 SQL、以及 InnoDB 回滚了哪一个
2. 开 `innodb_print_all_deadlocks=ON` 把所有死锁记进错误日志（默认只保留最近一次）
3. `innodb_lock_wait_timeout` 默认 50 秒，通常应调小到 5~10 秒

**重要认知：死锁不可能完全避免，应用层必须有重试逻辑。** InnoDB 会自动检测并回滚其中一个（报 1213 Deadlock found），你的代码应该捕获它并**重试整个事务**（不是重试单条 SQL）。

## 9. 日志系统

| | 谁产生 | 记录什么 | 用途 |
|---|---|---|---|
| **redo log** | InnoDB 引擎 | **物理**：某页某偏移改成什么 | 崩溃恢复（D）。循环写，固定大小 |
| **undo log** | InnoDB 引擎 | **逻辑**：反向操作 | 回滚（A）+ MVCC 版本链 |
| **binlog** | MySQL Server（引擎无关） | **逻辑**：SQL 或行变更 | 主从复制、时间点恢复。追加写 |

### WAL：Write-Ahead Logging

修改数据时**先写日志，再改数据页**。为什么更快？

- 改数据页是**随机**写（页散布在磁盘各处）
- 写 redo log 是**顺序**追加

顺序写比随机写快 1~2 个数量级。于是：先顺序写日志保证不丢，脏页慢慢在后台刷；崩溃了就重放 redo log。

### 两阶段提交

redo log 是引擎层的，binlog 是 Server 层的，两者必须一致，否则主从数据会不一致：

- 只写 redo 不写 binlog 就崩溃 → 主库有这条数据，从库靠 binlog 复制 → 没有
- 只写 binlog 不写 redo 就崩溃 → 主库没有，从库有

所以提交流程是：

1. 写 redo log，标记为 **prepare**
2. 写 binlog 并 fsync
3. 写 redo log，标记为 **commit**

崩溃恢复的判定规则：

| redo 状态 | binlog | 动作 |
|---|---|---|
| commit | — | 提交 |
| prepare | 完整 | **提交**（因为从库会执行它） |
| prepare | 不完整 | 回滚 |

### 两个关键参数（数据安全 vs 性能的核心权衡）

`innodb_flush_log_at_trx_commit`：

| 值 | 行为 | 风险 |
|---|---|---|
| **1**（默认） | 每次提交都 fsync redo | 最安全，掉电不丢 |
| 2 | 每次写 OS 缓存，每秒 fsync | MySQL 崩溃不丢，掉电丢 1 秒 |
| 0 | 每秒才写并 fsync | 崩溃就丢 1 秒 |

`sync_binlog`：1 = 每次提交都 fsync（默认最安全）；0 = 交给 OS；N = 每 N 次提交 fsync。

"**双 1**"配置是金融级标准，性能损失明显；很多互联网业务用 `(2, 1000)` 换吞吐。**这是个业务决策，不是技术最优解问题。**

### binlog 三种格式

| 格式 | 特点 |
|---|---|
| STATEMENT | 记 SQL 原文。日志小，但 `NOW()`、`UUID()`、无 ORDER BY 的 LIMIT 会导致主从不一致 |
| **ROW** | 记每一行的前后镜像。安全，**现在的推荐值**。缺点是一条影响百万行的 UPDATE 产生百万条记录 |
| MIXED | 平时 STATEMENT，遇到不确定语句自动切 ROW |

## 10. Buffer Pool 与改进版 LRU

Buffer Pool 通常配置为物理内存的 50~75%，是 MySQL **最重要**的性能参数：命中率从 95% 掉到 90%，磁盘 IO 就翻倍。

### 为什么不能用朴素 LRU

**问题 1：预读失效。** InnoDB 会线性预读（访问一个区里的多个页时把整个区 64 页读进来）。预读的页若最终没被访问，却占在 LRU 头部，把真正的热数据挤到尾部。

**问题 2：缓冲池污染（更严重）。** 一条 `SELECT * FROM big_table` 读进几百万个页，每个只用一次。朴素 LRU 会把**所有热数据全部淘汰**。扫描结束后命中率归零，线上响应时间瞬间飙升。

### InnoDB 的解法：young / old 分区

```
┌──────────────── young (默认 5/8) ────────────┬──── old (3/8) ────┐
│ 热数据                                       │ 新读入的页在这里  │
└──────────────────────────────────────────────┴───────────────────┘
  ↑ head                                          midpoint      tail ↑
                                                                淘汰端
```

**规则 1**：新页插入到 **midpoint**（old 区头部），不是链表头 → 全表扫描的页只在 old 区打转，动不到 young 区的热数据。

**规则 2**：old 区的页被再次访问时，只有距首次访问超过 `innodb_old_blocks_time`（默认 1000 ms）才提升到 young 区 → 全表扫描时同一页在短时间内被连续访问（一页有多行），但间隔 < 1 秒，所以**不会**被提升。

### 程序实测

缓冲池 100 页，30 个热页，全表扫描 5000 个冷页：

| 阶段 | 朴素 LRU | InnoDB LRU |
|---|---|---|
| 建立热点后 | 30/30 | 30/30（30 次 old→young 提升） |
| **全表扫描后** | **0/30** | **30/30** |
| 业务恢复期命中率 | 95.0% | **100.0%** |

扫描期间 InnoDB 的额外提升次数 = **0** —— 两条规则完美挡住了扫描。

朴素 LRU 的热数据要重新从磁盘加载一遍才能恢复。在真实系统里这就是"**跑了一条没加索引的大查询，之后几分钟所有接口都变慢**"的直接原因。

（实现细节：old 区必须是当前链表长度的**比例**，不能是固定条数。写成固定条数时，缓冲池还没填满的情况下 midpoint 会落到靠前位置，新页被插进热数据中间，防污染完全失效 —— 本章开发时真踩了这个坑。）

### 相关参数与监控

```sql
-- 命中率 = 1 - Innodb_buffer_pool_reads / Innodb_buffer_pool_read_requests
SHOW STATUS LIKE 'Innodb_buffer_pool_read%';
```

生产环境应 > 99%。低于 95% 就该考虑加内存或优化 SQL。

| 参数 | 说明 |
|---|---|
| `innodb_buffer_pool_size` | 最重要，物理内存的 50~75% |
| `innodb_buffer_pool_instances` | 分成多个实例减少内部锁竞争（池 > 1 GB 时建议 8） |
| `innodb_old_blocks_pct` | old 区占比，默认 37 |
| `innodb_old_blocks_time` | 默认 1000 ms，防污染的关键 |

## 11. EXPLAIN 与优化流程

### type —— 最该先看的列（从好到坏）

| type | 含义 |
|---|---|
| `system` / `const` | 通过主键或唯一索引等值匹配，最多一行。最快 |
| `eq_ref` | JOIN 时用主键/唯一索引匹配 |
| `ref` | 用非唯一索引等值匹配，返回多行 |
| `range` | 索引范围扫描（BETWEEN / > / IN） |
| `index` | 扫描整棵索引树（比 ALL 好，索引比数据小） |
| **`ALL`** | **全表扫描 ← 看到这个就要警觉** |

**实践底线：线上查询至少要到 `range`，理想是 `ref` 或更好。** 例外：小表（几百行）全表扫反而更快。

### 其它关键列

- **`possible_keys` 有值但 `key` 是 NULL** → 优化器主动放弃了，通常是选择性太差或统计信息过期（试 `ANALYZE TABLE`）
- **`key_len`** → 用它判断联合索引用到了几列！计算规则：INT=4，BIGINT=8，可为 NULL 的列 +1，VARCHAR(n) utf8mb4 = 4n+2
- **`rows` × `filtered` / 100** ≈ 实际参与后续操作的行数

### Extra —— 信息量最大的一列

| 值 | 含义 |
|---|---|
| `Using index` | **覆盖索引，不回表。好** |
| `Using index condition` | 索引条件下推（ICP），在引擎层就过滤，好 |
| `Using where` | 用 WHERE 过滤了从表里取出的行 |
| `Using filesort` | **需要额外排序，要优化**（不一定用磁盘，内存排序也叫这名字） |
| `Using temporary` | **用了临时表，要优化**（常见于 GROUP BY / DISTINCT） |
| `Using join buffer` | JOIN 没用上索引，退化成 BNL 算法，要优化 |

进阶：`EXPLAIN FORMAT=JSON`（带 cost 估算）；**`EXPLAIN ANALYZE`**（8.0.18+，**真正执行**并给出实际耗时，比估算可信得多）。

### 慢查询优化的标准流程

**1. 定位** — 慢查询日志 → `pt-query-digest` → 找总耗时占比最高的语句。

排序依据是"**总耗时**"而不是"单次耗时"：一条 10 ms 但每秒执行 1000 次的 SQL，比一条 5 秒但每天跑一次的更值得优化。

**2. 分析** — `EXPLAIN` 看 type / key / rows / Extra。

**3. 优化（按性价比排序）**

- **a) 加/调索引** —— 最常见，收益最大
  - 等值列在联合索引前面，范围列放最后
  - `ORDER BY` 的列跟在等值列后面，可消除 filesort
  - 把 SELECT 的列并入索引 → 覆盖索引，消除回表
- **b) 改写 SQL**
  - `SELECT *` 改成只取需要的列（可能变成覆盖索引）
  - 深分页改 keyset 分页
  - 大批量 UPDATE/DELETE 拆成小批次（减少锁范围和主从延迟）
- **c) 调整表结构** —— 拆冷热字段、适度反范式
- **d) 加缓存 / 读写分离 / 分库分表** ← **最后才考虑**，引入的复杂度远大于前三项

**4. 验证** — `EXPLAIN ANALYZE` 对比前后实际耗时。**别忘了看写入是否变慢了**（每个索引都会拖慢 INSERT/UPDATE）。

### 常见误区纠正

| 误区 | 事实 |
|---|---|
| 索引越多越好 | 每个索引占空间、拖慢写入。单表建议不超过 5~6 个 |
| `COUNT(1)` 比 `COUNT(*)` 快 | **完全一样**。InnoDB 对 `COUNT(*)` 有专门优化。`COUNT(列)` 才不同 —— 它跳过 NULL，语义都不一样 |
| 查主键一定最快 | `SELECT * WHERE id IN (1000 个 id)` 是 1000 次随机 IO，可能比一次范围扫描慢 |
| JOIN 一定比子查询慢（或反之） | 取决于具体情况和版本。8.0 对半连接优化好了很多。结论只能靠 `EXPLAIN ANALYZE` 实测 |
| 加了索引就一定会用 | 见第 5 节的 8 种失效场景 |

### 为什么 InnoDB 的 COUNT(*) 慢

因为 **MVCC**：不同事务看到的行数可能不同（有的行对你可见，对我不可见），没法维护一个全局计数器。MyISAM 没有 MVCC，所以能存一个准确的总数。

优化：`COUNT(*)` 会自动选择最小的二级索引扫描；业务上通常用 Redis 或单独的计数表维护近似值。

## 本章要点

1. **B+ 树**：非叶子只存键 → 扇出大 → 树高 3 层存千万行 → 1~3 次 IO；叶子链表让范围查询变顺序读
2. InnoDB 表就是聚簇索引；二级索引叶子存主键 → **回表**；**覆盖索引**消除回表。主键要短且自增
3. 页 16 KB 是 IO 最小单位；单行别超 8 KB
4. **最左前缀**：联合索引按字典序排；范围列放最后
5. 索引失效的头号隐蔽杀手是**隐式类型转换**（VARCHAR 列传数字）
6. ACID = undo(A) + redo(D) + 锁与 MVCC(I)
7. **MVCC** 让读不加锁：版本链 + ReadView 四步判断。**RC 与 RR 的唯一区别是 ReadView 何时创建**
8. 锁加在**索引**上；RR 默认 next-key lock（左开右闭）防幻读。**死锁无法避免，应用必须重试整个事务**
9. WAL + 两阶段提交保证 redo 与 binlog 一致
10. Buffer Pool 的 **young/old 分区**专门防全表扫描污染缓存
11. 优化先看 `EXPLAIN` 的 `type` 和 `Extra`；**加索引 > 改写 SQL > 改表结构 > 加缓存分库**
