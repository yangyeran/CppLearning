// =============================================================================
// 第 17 章 —— MySQL：常用命令 + 实现原理
//
// 本章不需要安装 MySQL。原理部分全部用可运行的 C++ 模拟实现，
// 你能亲眼看到 B+ 树怎么长、MVCC 怎么判断可见性、Buffer Pool 怎么淘汰页。
//
// 目录：
//   17.1  关系模型与 SQL 分类
//   17.2  常用命令速查（DDL / DML / DQL / 运维）
//   17.3  存储引擎：InnoDB vs MyISAM
//   17.4  为什么索引用 B+ 树 —— 手写 B+ 树 + 磁盘 IO 次数计算
//   17.5  聚簇索引 / 二级索引 / 回表 / 覆盖索引
//   17.6  InnoDB 页与行格式
//   17.7  索引失效的 8 种场景 + 最左前缀原则
//   17.8  事务 ACID 与隔离级别
//   17.9  MVCC —— 手写版本链 + ReadView 可见性判断
//   17.10 锁：行锁 / 间隙锁 / next-key lock / 死锁
//   17.11 redo log / undo log / binlog 与两阶段提交
//   17.12 Buffer Pool 与改进版 LRU
//   17.13 EXPLAIN 解读与慢查询优化流程
//
// 运行： ch17_mysql.exe
// =============================================================================

#include "demo.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// =============================================================================
// 17.1  关系模型与 SQL 分类
// =============================================================================
static void s01_relational_model() {
    demo::title("17.1  关系模型与 SQL 分类");

    std::cout <<
        "  关系模型的三个基本概念：\n"
        "    关系 (relation)  = 表\n"
        "    元组 (tuple)     = 行 / 记录\n"
        "    属性 (attribute) = 列 / 字段\n"
        "\n"
        "  SQL 按功能分四类（面试常问的分类题）：\n"
        "\n"
        "    DDL  Data Definition Language     定义结构\n"
        "         CREATE / ALTER / DROP / TRUNCATE\n"
        "         注意：DDL 在 MySQL 8.0 之前**不支持事务回滚**，\n"
        "               执行 DDL 会隐式提交当前事务。\n"
        "\n"
        "    DML  Data Manipulation Language   操作数据\n"
        "         INSERT / UPDATE / DELETE\n"
        "\n"
        "    DQL  Data Query Language         查询\n"
        "         SELECT\n"
        "\n"
        "    DCL  Data Control Language       权限与事务\n"
        "         GRANT / REVOKE / COMMIT / ROLLBACK / SAVEPOINT\n"
        "\n"
        "  三大范式（了解即可，实际业务经常为性能反范式）：\n"
        "    1NF  每列不可再分（不要在一列里存逗号分隔的多个值）\n"
        "    2NF  非主键列完全依赖主键（消除部分依赖）\n"
        "    3NF  非主键列不依赖其它非主键列（消除传递依赖）\n"
        "\n"
        "  反范式的典型场景：订单表里冗余一份「下单时的商品名和价格」。\n"
        "  因为商品名会改、价格会变，订单必须记录**当时的快照**，\n"
        "  这不是冗余错误，而是业务正确性要求。\n";

    demo::section("SQL 的逻辑执行顺序（与书写顺序不同，这是理解 SQL 的关键）");
    std::cout <<
        "  书写顺序：\n"
        "    SELECT ... FROM ... JOIN ... ON ... WHERE ... GROUP BY ...\n"
        "    HAVING ... ORDER BY ... LIMIT ...\n"
        "\n"
        "  实际执行顺序：\n"
        "    1. FROM / JOIN     确定数据来源，生成笛卡尔积\n"
        "    2. ON              连接条件过滤\n"
        "    3. WHERE           行级过滤（此时还没有分组，用不了聚合函数）\n"
        "    4. GROUP BY        分组\n"
        "    5. 聚合函数        COUNT / SUM / AVG 在这一步计算\n"
        "    6. HAVING          组级过滤（可以用聚合函数）\n"
        "    7. SELECT          选出列、计算表达式、赋别名\n"
        "    8. DISTINCT        去重\n"
        "    9. ORDER BY        排序（可以用 SELECT 里的别名，因为第 7 步已完成）\n"
        "   10. LIMIT           取前 N 条\n"
        "\n"
        "  由此可解释两个高频困惑：\n"
        "    - 为什么 WHERE 里不能用 COUNT()？  因为 WHERE 在聚合之前执行\n"
        "    - 为什么 WHERE 里不能用 SELECT 的别名，ORDER BY 却可以？\n"
        "      因为 WHERE(3) 早于 SELECT(7)，ORDER BY(9) 晚于 SELECT(7)\n";
}

// =============================================================================
// 17.2  常用命令速查
// =============================================================================
static void s02_commands() {
    demo::title("17.2  常用命令速查");

    demo::section("连接与库表管理");
    std::cout <<
        "  mysql -h 127.0.0.1 -P 3306 -u root -p            登录（-p 后不要跟密码）\n"
        "  mysql -u root -p dbname < backup.sql             导入\n"
        "  mysqldump -u root -p --single-transaction db > b.sql   备份（InnoDB 热备）\n"
        "\n"
        "  SHOW DATABASES;                                  列出所有库\n"
        "  CREATE DATABASE shop\n"
        "    DEFAULT CHARACTER SET utf8mb4\n"
        "    COLLATE utf8mb4_0900_ai_ci;                    建库（必须 utf8mb4）\n"
        "  USE shop;                                        切换库\n"
        "  DROP DATABASE shop;                              删库\n"
        "\n"
        "  为什么必须 utf8mb4 而不是 utf8？\n"
        "    MySQL 的 \"utf8\" 是历史遗留的**残缺**实现，每字符最多 3 字节，\n"
        "    存不了 emoji 和部分生僻汉字（它们需要 4 字节）。\n"
        "    utf8mb4 才是真正的 UTF-8。老库用 utf8 存 emoji 会直接报错或截断。\n";

    demo::section("DDL —— 表结构");
    std::cout <<
        "  CREATE TABLE orders (\n"
        "      id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,\n"
        "      user_id     BIGINT UNSIGNED NOT NULL,\n"
        "      order_no    VARCHAR(32)     NOT NULL,\n"
        "      amount      DECIMAL(12,2)   NOT NULL DEFAULT 0.00,\n"
        "      status      TINYINT         NOT NULL DEFAULT 0,\n"
        "      created_at  DATETIME(3)     NOT NULL DEFAULT CURRENT_TIMESTAMP(3),\n"
        "      updated_at  DATETIME(3)     NOT NULL DEFAULT CURRENT_TIMESTAMP(3)\n"
        "                                  ON UPDATE CURRENT_TIMESTAMP(3),\n"
        "      PRIMARY KEY (id),\n"
        "      UNIQUE  KEY uk_order_no (order_no),\n"
        "      KEY         idx_user_status_created (user_id, status, created_at)\n"
        "  ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='订单表';\n"
        "\n"
        "  这段建表语句里的每个决定都有理由：\n"
        "    BIGINT UNSIGNED AUTO_INCREMENT  自增主键 -> 顺序插入，B+ 树不分裂\n"
        "    DECIMAL(12,2) 存金额            绝不用 FLOAT/DOUBLE（见第 13 章坑 06）\n"
        "    NOT NULL + DEFAULT              NULL 让索引统计和条件判断都变复杂\n"
        "    DATETIME(3)                     带毫秒；DATETIME 不含时区，\n"
        "                                    TIMESTAMP 含时区但只到 2038 年\n"
        "    联合索引列的顺序                 见 17.7 最左前缀原则\n"
        "\n"
        "  ALTER TABLE orders ADD COLUMN remark VARCHAR(255) NULL;\n"
        "  ALTER TABLE orders MODIFY COLUMN status SMALLINT NOT NULL DEFAULT 0;\n"
        "  ALTER TABLE orders ADD INDEX idx_created (created_at);\n"
        "  ALTER TABLE orders DROP INDEX idx_created;\n"
        "  ALTER TABLE orders RENAME TO shop_orders;\n"
        "\n"
        "  DESC orders;                    查看表结构\n"
        "  SHOW CREATE TABLE orders;       查看完整建表语句（含索引和字符集）\n"
        "  SHOW INDEX FROM orders;         查看索引详情（含基数 Cardinality）\n"
        "\n"
        "  【生产环境警告】大表 ALTER 会锁表或长时间占用资源。\n"
        "  MySQL 5.6+ 支持 Online DDL，但并非所有操作都支持：\n"
        "    ALGORITHM=INSTANT   秒级完成（8.0，加列在末尾）\n"
        "    ALGORITHM=INPLACE   不拷表，但可能重建索引\n"
        "    ALGORITHM=COPY      拷全表，最慢，期间阻塞写\n"
        "  写法：ALTER TABLE t ADD COLUMN c INT, ALGORITHM=INSTANT, LOCK=NONE;\n"
        "  大表变更推荐 gh-ost 或 pt-online-schema-change。\n";

    demo::section("DML / DQL");
    std::cout <<
        "  INSERT INTO orders (user_id, order_no, amount) VALUES (1, 'A001', 99.50);\n"
        "  INSERT INTO orders (user_id, order_no) VALUES (1,'A2'),(2,'A3'),(3,'A4');  批量\n"
        "  INSERT INTO orders (...) VALUES (...)\n"
        "      ON DUPLICATE KEY UPDATE amount = VALUES(amount);       存在则更新\n"
        "  REPLACE INTO orders (...) VALUES (...);   先删后插（会改变自增 id，慎用）\n"
        "\n"
        "  UPDATE orders SET status = 1 WHERE id = 100;\n"
        "  DELETE FROM orders WHERE created_at < '2024-01-01' LIMIT 1000;   分批删\n"
        "\n"
        "  DELETE / TRUNCATE / DROP 的区别（高频面试题）：\n"
        "    DELETE    DML，逐行删除，写 undo log，可回滚，触发触发器，\n"
        "              不释放空间（只标记删除），自增值不重置\n"
        "    TRUNCATE  DDL，直接重建表空间文件，极快，不可回滚，\n"
        "              不触发触发器，释放空间，自增值重置为 1\n"
        "    DROP      DDL，删除表结构 + 数据 + 索引 + 权限\n"
        "\n"
        "  SELECT ... 常用写法：\n"
        "    SELECT COUNT(*) FROM orders;                     统计行数\n"
        "    SELECT user_id, SUM(amount) AS total\n"
        "      FROM orders WHERE status = 1\n"
        "      GROUP BY user_id HAVING total > 1000\n"
        "      ORDER BY total DESC LIMIT 10;\n"
        "\n"
        "    -- 分页：深分页的正确写法\n"
        "    SELECT * FROM orders ORDER BY id LIMIT 1000000, 20;    -- 慢！\n"
        "    SELECT * FROM orders WHERE id > 1000000 ORDER BY id LIMIT 20;  -- 快\n"
        "    原因：LIMIT 1000000, 20 要先扫描并丢弃前 100 万行；\n"
        "          用「上次的最大 id」做游标可以直接定位，这叫 keyset 分页。\n"
        "\n"
        "    -- JOIN 的四种\n"
        "    INNER JOIN   两边都有才出现\n"
        "    LEFT  JOIN   左表全保留，右表无匹配则补 NULL\n"
        "    RIGHT JOIN   反之（实践中一律改写成 LEFT JOIN，可读性更好）\n"
        "    CROSS JOIN   笛卡尔积\n"
        "    MySQL **不支持** FULL OUTER JOIN，要用 UNION 模拟\n"
        "\n"
        "    -- 窗口函数（8.0+），取每组前 N 条的标准解法\n"
        "    SELECT * FROM (\n"
        "        SELECT *, ROW_NUMBER() OVER (\n"
        "                     PARTITION BY user_id ORDER BY created_at DESC) AS rn\n"
        "        FROM orders\n"
        "    ) t WHERE rn <= 3;\n"
        "\n"
        "    -- CTE 公用表表达式（8.0+），替代嵌套子查询，可读性大幅提升\n"
        "    WITH recent AS (\n"
        "        SELECT * FROM orders WHERE created_at > '2026-01-01'\n"
        "    )\n"
        "    SELECT user_id, COUNT(*) FROM recent GROUP BY user_id;\n";

    demo::section("事务");
    std::cout <<
        "  START TRANSACTION;              -- 或 BEGIN\n"
        "      UPDATE account SET balance = balance - 100 WHERE id = 1;\n"
        "      UPDATE account SET balance = balance + 100 WHERE id = 2;\n"
        "  COMMIT;                         -- 或 ROLLBACK\n"
        "\n"
        "  SAVEPOINT sp1;                  部分回滚点\n"
        "  ROLLBACK TO SAVEPOINT sp1;\n"
        "\n"
        "  SELECT @@transaction_isolation;             查看隔离级别\n"
        "  SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ;\n"
        "\n"
        "  SELECT ... FOR UPDATE;          加排他行锁（写锁）\n"
        "  SELECT ... FOR SHARE;           加共享行锁（8.0；5.7 是 LOCK IN SHARE MODE）\n"
        "  SELECT ... FOR UPDATE NOWAIT;         锁不到立刻报错（8.0）\n"
        "  SELECT ... FOR UPDATE SKIP LOCKED;    跳过被锁的行（8.0，做队列很有用）\n";

    demo::section("运维与诊断");
    std::cout <<
        "  SHOW PROCESSLIST;                       当前连接与正在执行的语句\n"
        "  SHOW FULL PROCESSLIST;                  不截断 SQL 文本\n"
        "  KILL <id>;                              杀掉某个连接\n"
        "\n"
        "  SHOW ENGINE INNODB STATUS;              InnoDB 全量状态（含最近一次死锁！）\n"
        "  SHOW VARIABLES LIKE 'innodb_buffer_pool_size';\n"
        "  SHOW STATUS LIKE 'Threads_connected';\n"
        "\n"
        "  -- 慢查询日志\n"
        "  SET GLOBAL slow_query_log = ON;\n"
        "  SET GLOBAL long_query_time = 1;              超过 1 秒记录\n"
        "  SET GLOBAL log_queries_not_using_indexes = ON;\n"
        "  然后用 mysqldumpslow 或 pt-query-digest 分析\n"
        "\n"
        "  -- 表空间与行数估算\n"
        "  SELECT table_name,\n"
        "         table_rows,\n"
        "         ROUND(data_length/1024/1024, 2)  AS data_mb,\n"
        "         ROUND(index_length/1024/1024, 2) AS index_mb\n"
        "  FROM information_schema.tables\n"
        "  WHERE table_schema = 'shop' ORDER BY data_length DESC;\n"
        "  注意 table_rows 是**估算值**，精确值要 COUNT(*)。\n"
        "\n"
        "  -- 锁等待排查（8.0）\n"
        "  SELECT * FROM performance_schema.data_locks;\n"
        "  SELECT * FROM performance_schema.data_lock_waits;\n"
        "  -- 5.7 用 information_schema.innodb_locks / innodb_lock_waits\n"
        "\n"
        "  ANALYZE TABLE orders;      重新采样索引统计信息（执行计划不准时用）\n"
        "  OPTIMIZE TABLE orders;     重建表回收碎片空间（InnoDB 等价于 ALTER 重建）\n";
}

// =============================================================================
// 17.3  存储引擎
// =============================================================================
static void s03_engines() {
    demo::title("17.3  存储引擎：InnoDB vs MyISAM");

    std::cout <<
        "                        InnoDB              MyISAM\n"
        "  事务                   支持                不支持\n"
        "  外键                   支持                不支持\n"
        "  锁粒度                 行锁                表锁\n"
        "  崩溃恢复               支持（redo log）    不支持（要 repair）\n"
        "  MVCC                   支持                不支持\n"
        "  索引结构               聚簇索引            非聚簇（索引与数据分离）\n"
        "  COUNT(*)               需要扫描            直接读计数器（O(1)）\n"
        "  文件                   .ibd（表空间）      .MYD + .MYI\n"
        "\n"
        "  MySQL 5.5 起默认 InnoDB。**新项目没有理由选 MyISAM**，\n"
        "  它唯一的优势是 COUNT(*) 快和表更小，代价是没有事务和崩溃恢复。\n"
        "\n"
        "  为什么 InnoDB 的 COUNT(*) 慢？\n"
        "    因为 MVCC：不同事务看到的行数可能不同（有的行对你可见，对我不可见），\n"
        "    没法维护一个全局计数器。MyISAM 没有 MVCC，所以能存一个准确的总数。\n"
        "    优化：COUNT(*) 会自动选择最小的二级索引扫描，比扫聚簇索引快；\n"
        "          业务上通常用 Redis 或单独的计数表维护近似值。\n";
}

// =============================================================================
// 17.4  为什么索引用 B+ 树 —— 手写实现
//
// 候选方案的淘汰过程：
//
//   哈希表      O(1) 等值查找，但**不支持范围查询和排序**。
//               WHERE age > 20、ORDER BY age 全部失效。
//               InnoDB 内部只在自适应哈希索引 (AHI) 里用它做等值加速。
//
//   二叉搜索树  有序，但树高 O(log₂n)。100 万行 -> 树高 20，
//               每层一次磁盘 IO -> 20 次 IO。太慢。
//
//   红黑树      同上，仍是二叉，扇出只有 2。
//
//   B 树        多路平衡，扇出大 -> 树高低。但**非叶子节点也存数据**，
//               导致每个节点能放的索引项变少 -> 扇出下降 -> 树更高。
//               且范围查询要中序遍历，来回跳节点。
//
//   B+ 树       ✓ 非叶子节点**只存键**，不存数据 -> 一页能放几百个键
//               ✓ 所有数据都在叶子层，且叶子之间用双向链表连接
//                 -> 范围查询就是顺序扫链表，磁盘顺序读，极快
//               ✓ 树高稳定在 3 层就能存千万级数据
//
// 关键计算（面试常考）：
//   InnoDB 页大小 16 KB。
//   非叶子节点：每项 = 键(8B, BIGINT) + 页号(6B) = 14 B
//               扇出 ≈ 16384 / 14 ≈ 1170
//   叶子节点：假设每行 1 KB -> 每页放 16 行
//
//   树高 2（1 层非叶 + 1 层叶）：1170 × 16      ≈ 1.87 万行
//   树高 3（2 层非叶 + 1 层叶）：1170×1170×16   ≈ 2190 万行
//   树高 4：                    1170³×16       ≈ 256 亿行
//
//   所以「千万级表的索引查找只需 3 次磁盘 IO」，而且根节点常驻内存，
//   实际只有 1~2 次真实 IO。这就是 B+ 树的威力。
// =============================================================================

namespace mysql_sim {

// 简化的 B+ 树。order = 每个节点最多的键数。
// 真实 InnoDB 的 order 由页大小和键大小决定（约 1170），这里用小值方便观察分裂。
template <typename K, typename V, int Order = 4>
class BPlusTree {
    static_assert(Order >= 3, "order 至少 3");

    struct Node {
        bool                  isLeaf;
        std::vector<K>        keys;
        // 非叶子节点用 children；叶子节点用 values + next
        std::vector<Node*>    children;
        std::vector<V>        values;
        Node*                 next = nullptr;   // 叶子层链表（范围查询用）
        Node*                 prev = nullptr;
        explicit Node(bool leaf) : isLeaf(leaf) {}
    };

    Node*  root_ = nullptr;
    size_t size_ = 0;
    int    height_ = 0;
    // 统计「访问了多少个节点」——模拟磁盘 IO 次数
    mutable long long nodeVisits_ = 0;

public:
    BPlusTree() : root_(new Node(true)), height_(1) {}
    ~BPlusTree() { destroy(root_); }
    BPlusTree(const BPlusTree&) = delete;
    BPlusTree& operator=(const BPlusTree&) = delete;

    void insert(const K& key, const V& value) {
        Node* r = root_;
        if (static_cast<int>(r->keys.size()) == Order) {
            // 根节点满了 -> 长高一层（B+ 树是**从根部向上**长高的）
            Node* newRoot = new Node(false);
            newRoot->children.push_back(r);
            splitChild(newRoot, 0);
            root_ = newRoot;
            ++height_;
        }
        insertNonFull(root_, key, value);
        ++size_;
    }

    std::optional<V> find(const K& key) const {
        Node* n = root_;
        nodeVisits_ = 0;
        while (!n->isLeaf) {
            ++nodeVisits_;                       // 每下降一层 = 一次页访问
            size_t i = 0;
            while (i < n->keys.size() && key >= n->keys[i]) ++i;
            n = n->children[i];
        }
        ++nodeVisits_;
        for (size_t i = 0; i < n->keys.size(); ++i) {
            if (n->keys[i] == key) return n->values[i];
        }
        return std::nullopt;
    }

    // 范围查询：这就是 B+ 树相对 B 树的杀手级优势。
    // 先定位到起点叶子，然后**顺着叶子链表走**，不用回到上层。
    std::vector<std::pair<K, V>> range(const K& lo, const K& hi) const {
        std::vector<std::pair<K, V>> out;
        Node* n = root_;
        nodeVisits_ = 0;
        while (!n->isLeaf) {                     // 定位阶段：树高次页访问
            ++nodeVisits_;
            size_t i = 0;
            while (i < n->keys.size() && lo >= n->keys[i]) ++i;
            n = n->children[i];
        }
        while (n) {                              // 扫描阶段：顺序读叶子链表
            ++nodeVisits_;
            for (size_t i = 0; i < n->keys.size(); ++i) {
                if (n->keys[i] > hi) return out;
                if (n->keys[i] >= lo) out.emplace_back(n->keys[i], n->values[i]);
            }
            n = n->next;
        }
        return out;
    }

    size_t    size()   const { return size_; }
    int       height() const { return height_; }
    long long lastVisits() const { return nodeVisits_; }

    // 打印每层的节点数与键，直观看到结构
    void printStructure(int maxLevels = 4) const {
        std::vector<Node*> level{root_};
        int depth = 0;
        while (!level.empty() && depth < maxLevels) {
            std::cout << "    第 " << depth << " 层"
                      << (level[0]->isLeaf ? "（叶子）" : "（内部）")
                      << " 共 " << level.size() << " 个节点: ";
            int printed = 0;
            for (Node* n : level) {
                if (printed++ >= 4) { std::cout << "..."; break; }
                std::cout << "[";
                for (size_t i = 0; i < n->keys.size(); ++i) {
                    if (i) std::cout << ",";
                    std::cout << n->keys[i];
                }
                std::cout << "] ";
            }
            std::cout << "\n";

            std::vector<Node*> nextLevel;
            for (Node* n : level) {
                if (!n->isLeaf) {
                    for (Node* c : n->children) nextLevel.push_back(c);
                }
            }
            level = nextLevel;
            ++depth;
        }
    }

private:
    void destroy(Node* n) {
        if (!n) return;
        if (!n->isLeaf) for (Node* c : n->children) destroy(c);
        delete n;
    }

    // 分裂：把 child 的后一半搬到新节点，中间键上提给父节点
    void splitChild(Node* parent, size_t idx) {
        Node* child = parent->children[idx];
        int   mid    = Order / 2;

        Node* sibling = new Node(child->isLeaf);

        if (child->isLeaf) {
            // 叶子分裂：中间键**同时保留在右边**（因为叶子要存全部数据）
            sibling->keys.assign(child->keys.begin() + mid, child->keys.end());
            sibling->values.assign(child->values.begin() + mid, child->values.end());
            child->keys.resize(static_cast<size_t>(mid));
            child->values.resize(static_cast<size_t>(mid));

            // 维护叶子双向链表 —— 范围查询依赖它
            sibling->next = child->next;
            sibling->prev = child;
            if (child->next) child->next->prev = sibling;
            child->next = sibling;

            parent->keys.insert(parent->keys.begin() + static_cast<long>(idx),
                                sibling->keys.front());
        } else {
            // 内部节点分裂：中间键**上提**，不保留在子节点里
            sibling->keys.assign(child->keys.begin() + mid + 1, child->keys.end());
            sibling->children.assign(child->children.begin() + mid + 1, child->children.end());
            K upKey = child->keys[static_cast<size_t>(mid)];
            child->keys.resize(static_cast<size_t>(mid));
            child->children.resize(static_cast<size_t>(mid) + 1);

            parent->keys.insert(parent->keys.begin() + static_cast<long>(idx), upKey);
        }
        parent->children.insert(parent->children.begin() + static_cast<long>(idx) + 1, sibling);
    }

    void insertNonFull(Node* n, const K& key, const V& value) {
        if (n->isLeaf) {
            size_t i = 0;
            while (i < n->keys.size() && n->keys[i] < key) ++i;
            if (i < n->keys.size() && n->keys[i] == key) {
                n->values[i] = value;             // 键已存在，覆盖
                --size_;
                return;
            }
            n->keys.insert(n->keys.begin() + static_cast<long>(i), key);
            n->values.insert(n->values.begin() + static_cast<long>(i), value);
            return;
        }

        size_t i = 0;
        while (i < n->keys.size() && key >= n->keys[i]) ++i;
        if (static_cast<int>(n->children[i]->keys.size()) == Order) {
            splitChild(n, i);
            if (key >= n->keys[i]) ++i;
        }
        insertNonFull(n->children[i], key, value);
    }
};

} // namespace mysql_sim

static void s04_bplus_tree() {
    demo::title("17.4  为什么索引用 B+ 树");

    demo::section("手写 B+ 树：观察它如何分裂长高");
    {
        mysql_sim::BPlusTree<int, std::string, 4> tree;
        std::cout << "  依次插入 1..20（order=4，即每节点最多 4 个键）\n";
        for (int i = 1; i <= 20; ++i) tree.insert(i, "row" + std::to_string(i));
        std::cout << "  结果：size=" << tree.size() << "  树高=" << tree.height() << "\n";
        tree.printStructure();
        std::cout << "  注意：所有实际数据都在叶子层，内部节点只有导航用的键。\n";
    }

    demo::section("等值查找 vs 范围查找的页访问次数");
    {
        mysql_sim::BPlusTree<int, std::string, 8> tree;
        for (int i = 1; i <= 5000; ++i) tree.insert(i, "row" + std::to_string(i));

        auto v = tree.find(3777);
        std::cout << "  find(3777) = " << (v ? *v : "未找到")
                  << "  访问了 " << tree.lastVisits() << " 个节点（= 树高 "
                  << tree.height() << "）\n";

        auto r = tree.range(3000, 3050);
        std::cout << "  range(3000, 3050) 返回 " << r.size() << " 行，"
                  << "访问了 " << tree.lastVisits() << " 个节点\n";
        std::cout << "    其中 " << tree.height() << " 次用于定位起点，"
                  << "其余是沿叶子链表顺序扫描\n";
        std::cout << "    ^^^ 这就是 B+ 树范围查询快的原因：定位一次，然后顺序读。\n";
        std::cout << "        B 树没有叶子链表，范围查询要不断回到父节点，随机 IO。\n";
    }

    demo::section("InnoDB 真实参数下的容量计算");
    {
        const double pageSize     = 16.0 * 1024;   // InnoDB 页 16 KB
        const double keyPlusPtr   = 8 + 6;          // BIGINT 主键 + 页号
        const double fanout       = std::floor(pageSize / keyPlusPtr);

        std::cout << "  页大小 16 KB，主键 BIGINT(8B) + 页号(6B) = 14 B/项\n";
        std::cout << "  -> 非叶子节点扇出 = 16384 / 14 = " << static_cast<long long>(fanout)
                  << "\n\n";
        std::cout << "  " << demo::pad("每行大小", 12) << demo::pad("每页行数", 12)
                  << demo::pad("树高 2", 16) << demo::pad("树高 3", 18) << "树高 4\n";
        for (int rowSize : {100, 200, 500, 1000}) {
            double rowsPerPage = std::floor(pageSize / rowSize);
            double h2 = fanout * rowsPerPage;
            double h3 = fanout * fanout * rowsPerPage;
            double h4 = fanout * fanout * fanout * rowsPerPage;
            auto fmt = [](double n) -> std::string {
                if (n >= 1e8) return std::to_string(static_cast<long long>(n / 1e8)) + " 亿";
                if (n >= 1e4) return std::to_string(static_cast<long long>(n / 1e4)) + " 万";
                return std::to_string(static_cast<long long>(n));
            };
            std::cout << "  " << demo::pad(std::to_string(rowSize) + " B", 12)
                      << demo::pad(std::to_string(static_cast<int>(rowsPerPage)), 12)
                      << demo::pad(fmt(h2), 16)
                      << demo::pad(fmt(h3), 18)
                      << fmt(h4) << "\n";
        }
        std::cout <<
            "\n  结论：常规表（行 1 KB 以内）在树高 3 时就能存**千万到上亿**行。\n"
            "  而根节点和大部分非叶子节点常驻 Buffer Pool，\n"
            "  所以一次主键查找通常只有 0~1 次真实磁盘 IO。\n";
    }

    demo::section("对比：如果用红黑树做索引会怎样");
    {
        std::cout << "  " << demo::pad("行数", 14) << demo::pad("B+树(扇出1170)", 20)
                  << "红黑树(扇出2)\n";
        for (long long n : {10000LL, 1000000LL, 100000000LL}) {
            double bplus = std::ceil(std::log(static_cast<double>(n) / 16.0)
                                     / std::log(1170.0)) + 1;
            double rb    = std::ceil(std::log2(static_cast<double>(n)));
            std::string label = (n >= 100000000)
                                    ? std::to_string(n / 100000000) + " 亿"
                                    : std::to_string(n / 10000) + " 万";
            std::cout << "  " << demo::pad(label, 14)
                      << demo::pad(std::to_string(static_cast<int>(bplus)) + " 层", 20)
                      << static_cast<int>(rb) << " 层\n";
        }
        std::cout <<
            "\n  1 亿行：B+ 树 4 层，红黑树 27 层。\n"
            "  磁盘随机 IO 约 0.1 ms（SSD）到 10 ms（HDD），\n"
            "  27 次 IO 在 HDD 上就是 270 ms —— 一个查询走完就超时了。\n"
            "  内存里红黑树很好（std::map 就用它），但**磁盘索引必须降低树高**。\n";
    }
}

// =============================================================================
// 17.5  聚簇索引 / 二级索引 / 回表 / 覆盖索引
// =============================================================================
static void s05_clustered_index() {
    demo::title("17.5  聚簇索引与二级索引");

    std::cout <<
        "  InnoDB 的表**本身就是一棵 B+ 树**，这棵树叫聚簇索引（clustered index）。\n"
        "  它的叶子节点直接存放完整的行数据。\n"
        "\n"
        "  聚簇索引（主键 id）                二级索引（KEY idx_name(name)）\n"
        "  ┌───────────────────┐            ┌───────────────────┐\n"
        "  │   [10 | 50 | 90]  │  内部节点   │  [Bob | Tom]      │\n"
        "  └────┬──────┬───────┘            └────┬────────┬─────┘\n"
        "       │      │                         │        │\n"
        "  ┌────▼──┐ ┌─▼─────┐              ┌────▼──┐ ┌───▼───┐\n"
        "  │ id=10 │ │ id=50 │  叶子节点     │Alice  │ │Tom    │\n"
        "  │ 完整行│ │ 完整行│  存**数据**   │ id=50 │ │ id=10 │  存**主键值**\n"
        "  │name=..│ │name=..│              └───────┘ └───────┘\n"
        "  │age=.. │ │age=.. │\n"
        "  └───────┘ └───────┘\n"
        "\n"
        "  【回表 (bookmark lookup)】\n"
        "    SELECT * FROM t WHERE name = 'Tom';\n"
        "    1. 在 idx_name 上找到 'Tom' -> 得到 id=10\n"
        "    2. 拿 id=10 再去聚簇索引查一遍 -> 得到完整行\n"
        "    两棵树各走一遍，IO 翻倍。这第二步就叫「回表」。\n"
        "\n"
        "  【覆盖索引 (covering index)】—— 消除回表\n"
        "    SELECT id, name FROM t WHERE name = 'Tom';\n"
        "    需要的 id 和 name 都在 idx_name 里（name 是键，id 是值），\n"
        "    不用回表。EXPLAIN 的 Extra 列会显示 \"Using index\"。\n"
        "\n"
        "    实用技巧：如果某个查询高频且只要少数几列，\n"
        "    就把这几列加进索引变成覆盖索引：\n"
        "        KEY idx_name_age (name, age)   -- 现在 SELECT name, age 也不回表\n"
        "    代价：索引变大，写入变慢。典型的空间换时间。\n"
        "\n"
        "  【为什么主键必须短、且最好自增？】\n"
        "    1. **每个二级索引的叶子都存一份主键值**。主键用 UUID(36字节 varchar)\n"
        "       而不是 BIGINT(8字节)，10 个二级索引就多占 280 字节/行的空间。\n"
        "    2. 自增主键 = 顺序插入 = 总是往最右边的页追加，\n"
        "       页利用率高、几乎不分裂。\n"
        "       随机主键（UUID）= 随机插入 = 频繁页分裂 + 页内碎片，\n"
        "       实测写入性能差几倍。\n"
        "    3. 没有显式主键时，InnoDB 会：\n"
        "       先找第一个 NOT NULL 的 UNIQUE 索引当主键；\n"
        "       都没有就生成一个 6 字节的隐藏 ROW_ID —— 这个 ROW_ID 是\n"
        "       **全局共享**的自增值，高并发插入会成为竞争点。\n"
        "       所以：永远显式定义主键。\n"
        "\n"
        "    如果业务需要 UUID，推荐方案：内部用自增 BIGINT 主键，\n"
        "    UUID 作为带唯一索引的业务列；或用有序 UUID（UUIDv7 / 雪花 ID）。\n";
}

// =============================================================================
// 17.6  InnoDB 页与行格式
// =============================================================================
static void s06_page_format() {
    demo::title("17.6  InnoDB 页与行格式");

    std::cout <<
        "  页 (page) 是 InnoDB 磁盘与内存交换的**最小单位**，默认 16 KB。\n"
        "  即使你只读 1 行，也要把整个 16 KB 的页读进 Buffer Pool。\n"
        "\n"
        "  页内结构：\n"
        "    ┌────────────────────────┬────────┐\n"
        "    │ File Header            │  38 B  │ 页号、前后页号（构成双向链表）、校验和\n"
        "    ├────────────────────────┼────────┤\n"
        "    │ Page Header            │  56 B  │ 记录数、空闲空间、最大事务 id\n"
        "    ├────────────────────────┼────────┤\n"
        "    │ Infimum + Supremum     │  26 B  │ 两条虚拟的边界记录（最小/最大）\n"
        "    ├────────────────────────┼────────┤\n"
        "    │ User Records           │  变长  │ 实际的行，按插入顺序物理存放，\n"
        "    │                        │        │ 通过 next_record 单链表维持**逻辑有序**\n"
        "    ├────────────────────────┼────────┤\n"
        "    │ Free Space             │  变长  │\n"
        "    ├────────────────────────┼────────┤\n"
        "    │ Page Directory         │  变长  │ 稀疏槽位数组，页内二分查找用\n"
        "    ├────────────────────────┼────────┤\n"
        "    │ File Trailer           │   8 B  │ 校验和（与 header 对比检测半写）\n"
        "    └────────────────────────┴────────┘\n"
        "\n"
        "  两个细节值得记住：\n"
        "    1. 行在页内**不是**物理有序的，靠 next_record 链表逻辑有序。\n"
        "       所以行内更新长度变化时不用挪动其它行。\n"
        "    2. Page Directory 是稀疏的（每 4~8 条记录一个槽），\n"
        "       页内查找 = 先在目录里二分定位到槽，再沿链表线性走几步。\n"
        "\n"
        "  行格式 (ROW_FORMAT)，8.0 默认 DYNAMIC：\n"
        "    COMPACT     变长列超长时，页内留 768 字节前缀 + 20 字节溢出页指针\n"
        "    DYNAMIC     溢出时页内只留 20 字节指针，全部数据放溢出页（更优）\n"
        "    COMPRESSED  DYNAMIC + 页压缩（省空间，费 CPU）\n"
        "\n"
        "  每行的隐藏列（MVCC 的基础，17.9 会用到）：\n"
        "    DB_ROW_ID     6 B   无主键时的隐藏主键\n"
        "    DB_TRX_ID     6 B   最后修改这行的事务 id\n"
        "    DB_ROLL_PTR   7 B   指向 undo log 中的上一个版本\n"
        "\n"
        "  【为什么单行不能太大？】\n"
        "    InnoDB 要求一页至少能放 **2 行**（否则 B+ 树退化成链表），\n"
        "    所以单行最大约 16KB/2 = 8 KB。超过就必须用溢出页，\n"
        "    读这一行要额外的随机 IO。\n"
        "    实践建议：大文本 / JSON / 图片不要直接放行内，\n"
        "    单独开表或放对象存储，主表只存 id 或 URL。\n";
}

// =============================================================================
// 17.7  索引失效场景 + 最左前缀
// =============================================================================
static void s07_index_failure() {
    demo::title("17.7  索引失效的 8 种场景");

    std::cout <<
        "  假设有索引：KEY idx_abc (a, b, c)\n"
        "\n"
        "  【最左前缀原则】\n"
        "    联合索引的 B+ 树是按 (a, b, c) 的**字典序**排列的。\n"
        "    就像电话簿按「姓, 名」排序：\n"
        "      知道姓 -> 能快速定位（用到索引）\n"
        "      只知道名 -> 只能全本翻（索引失效）\n"
        "\n"
        "    WHERE a=1                     ✓ 用到 (a)\n"
        "    WHERE a=1 AND b=2             ✓ 用到 (a,b)\n"
        "    WHERE a=1 AND b=2 AND c=3     ✓ 用到 (a,b,c)  全覆盖\n"
        "    WHERE a=1 AND c=3             △ 只用到 (a)，c 无法用于定位\n"
        "                                    （8.0 有 Index Skip Scan 可能改善）\n"
        "    WHERE b=2                     ✗ 跳过了最左列 a，索引失效\n"
        "    WHERE b=2 AND c=3             ✗ 同上\n"
        "    WHERE a=1 AND b>2 AND c=3     △ a,b 用到；b 是范围查询，\n"
        "                                    **c 无法再用于索引定位**（只能过滤）\n"
        "                                    -> 范围列要放联合索引的最后\n"
        "\n"
        "    注意：WHERE b=2 AND a=1 是**可以**用到索引的！\n"
        "    优化器会自动调整条件顺序。「最左」指的是索引列的顺序，\n"
        "    不是 SQL 里书写的顺序。\n"
        "\n"
        "  ─────────────────────────────────────────────────────────────\n"
        "  失效场景 1：对索引列做运算或用函数\n"
        "    ✗ WHERE YEAR(created_at) = 2026\n"
        "    ✗ WHERE id + 1 = 100\n"
        "    ✓ WHERE created_at >= '2026-01-01' AND created_at < '2027-01-01'\n"
        "    ✓ WHERE id = 99\n"
        "    原因：索引里存的是原值，YEAR(x) 的结果没有索引。\n"
        "    (MySQL 8.0.13+ 支持函数索引：ALTER TABLE t ADD INDEX ((YEAR(created_at)));)\n"
        "\n"
        "  失效场景 2：隐式类型转换\n"
        "    表里 phone 是 VARCHAR：\n"
        "    ✗ WHERE phone = 13800138000      -- 数字！触发 CAST(phone AS SIGNED)\n"
        "    ✓ WHERE phone = '13800138000'    -- 加引号\n"
        "    这是**最隐蔽**的索引失效，代码里传参类型写错就中招，\n"
        "    而且不报错，只是慢 1000 倍。\n"
        "    反过来：int 列写 WHERE id = '99' 不会失效（转换发生在常量侧）。\n"
        "\n"
        "  失效场景 3：字符集/排序规则不一致的 JOIN\n"
        "    a.name (utf8mb4_general_ci) JOIN b.name (utf8mb4_0900_ai_ci)\n"
        "    -> 需要转换，索引失效。建库时统一字符集能避免。\n"
        "\n"
        "  失效场景 4：前导模糊匹配\n"
        "    ✗ WHERE name LIKE '%tom'      -- 前面是 %，无法定位起点\n"
        "    ✗ WHERE name LIKE '%tom%'\n"
        "    ✓ WHERE name LIKE 'tom%'      -- 后缀模糊可以用索引\n"
        "    需要「包含」搜索：用全文索引 (FULLTEXT) 或 ES。\n"
        "\n"
        "  失效场景 5：OR 连接的条件中有非索引列\n"
        "    ✗ WHERE a = 1 OR d = 2        -- d 无索引 -> 整个查询全表扫\n"
        "    ✓ 给 d 也建索引，或改写成 UNION ALL\n"
        "\n"
        "  失效场景 6：!= / <> / NOT IN / NOT EXISTS\n"
        "    通常失效（优化器判断扫索引再回表比全表扫更贵）。\n"
        "    但这**取决于选择性**：如果 a != 1 只排除 1% 的行，走索引反而更慢。\n"
        "\n"
        "  失效场景 7：IS NOT NULL（IS NULL 通常可以走索引）\n"
        "    InnoDB 的索引会存 NULL 值，所以 IS NULL 能用索引；\n"
        "    IS NOT NULL 命中范围太大，常被优化器放弃。\n"
        "\n"
        "  失效场景 8：优化器主动放弃（这不是 bug）\n"
        "    当预估命中行数超过全表的 20~30% 时，优化器认为\n"
        "    「随机回表 N 次」比「顺序全表扫」更贵，于是选全表扫。\n"
        "    统计信息不准导致的误判可以用 ANALYZE TABLE 修正，\n"
        "    或用 FORCE INDEX 强制（慎用，是把优化器的判断权拿走了）。\n";

    demo::section("索引选择性（决定该不该建索引）");
    {
        std::cout <<
            "  选择性 = 不同值的数量 / 总行数。越接近 1 越值得建索引。\n\n";
        struct Col { const char* name; long long distinct; long long total; };
        Col cols[] = {
            {"id (主键)",        1000000, 1000000},
            {"order_no",          999500, 1000000},
            {"user_id",            50000, 1000000},
            {"city",                 300, 1000000},
            {"status",                 5, 1000000},
            {"is_deleted",             2, 1000000},
            {"gender",                 2, 1000000},
        };
        std::cout << "  " << demo::pad("列", 18) << demo::pad("不同值", 12)
                  << demo::pad("选择性", 12) << "建索引？\n";
        for (const auto& c : cols) {
            double sel = static_cast<double>(c.distinct) / static_cast<double>(c.total);
            const char* verdict =
                sel > 0.9  ? "值得（唯一索引）" :
                sel > 0.05 ? "值得" :
                sel > 0.01 ? "看查询模式" : "单列不值得";
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(5) << sel;
            std::cout << "  " << demo::pad(c.name, 18)
                      << demo::pad(std::to_string(c.distinct), 12)
                      << demo::pad(ss.str(), 12) << verdict << "\n";
        }
        std::cout <<
            "\n  status / is_deleted 这类低选择性列**单独**建索引没意义\n"
            "  （命中 20 万行，回表 20 万次比全表扫还慢），\n"
            "  但放进联合索引的**非首列**很有用：\n"
            "      KEY idx_user_status (user_id, status)\n"
            "  先用高选择性的 user_id 把范围缩到几十行，再用 status 过滤。\n";
    }
}

// =============================================================================
// 17.8  事务 ACID 与隔离级别
// =============================================================================
static void s08_transactions() {
    demo::title("17.8  事务 ACID 与隔离级别");

    std::cout <<
        "  ACID 各自由什么机制保证（这是理解 InnoDB 的核心）：\n"
        "\n"
        "    A  原子性 Atomicity     undo log —— 回滚时用它把数据改回去\n"
        "    C  一致性 Consistency   上面三者 + 约束（外键/唯一/CHECK）共同保证\n"
        "    I  隔离性 Isolation     锁 + MVCC\n"
        "    D  持久性 Durability    redo log —— 崩溃后用它重放已提交的修改\n"
        "\n"
        "  三类并发问题：\n"
        "\n"
        "    脏读 (dirty read)       读到了别的事务**还没提交**的修改。\n"
        "                            对方一回滚，你读到的就是从未存在的数据。\n"
        "\n"
        "    不可重复读 (non-repeatable read)\n"
        "                            同一事务内两次读**同一行**，值不一样\n"
        "                            （别的事务提交了 UPDATE）。\n"
        "\n"
        "    幻读 (phantom read)     同一事务内两次执行**同一范围查询**，\n"
        "                            行数不一样（别的事务提交了 INSERT）。\n"
        "                            注意幻读针对的是「行的出现/消失」，\n"
        "                            不可重复读针对的是「已有行的值变化」。\n"
        "\n"
        "  四种隔离级别：\n"
        "\n"
        "    " << demo::pad("级别", 26) << demo::pad("脏读", 8)
                << demo::pad("不可重复读", 14) << "幻读\n"
        "    " << demo::pad("READ UNCOMMITTED", 26) << demo::pad("可能", 8)
                << demo::pad("可能", 14) << "可能\n"
        "    " << demo::pad("READ COMMITTED", 26) << demo::pad("不会", 8)
                << demo::pad("可能", 14) << "可能\n"
        "    " << demo::pad("REPEATABLE READ (默认)", 26) << demo::pad("不会", 8)
                << demo::pad("不会", 14) << "基本不会*\n"
        "    " << demo::pad("SERIALIZABLE", 26) << demo::pad("不会", 8)
                << demo::pad("不会", 14) << "不会\n"
        "\n"
        "  * 关于 RR 与幻读，这是最容易讲错的地方，说清楚：\n"
        "      - **快照读**（普通 SELECT）：靠 MVCC，整个事务用同一个 ReadView，\n"
        "        所以看不到别人新插入的行 -> 无幻读。\n"
        "      - **当前读**（SELECT FOR UPDATE / UPDATE / DELETE）：读最新版本，\n"
        "        靠 next-key lock（行锁 + 间隙锁）锁住范围来防止插入 -> 无幻读。\n"
        "      - 所以 InnoDB 的 RR **确实解决了**幻读，这与教科书上\n"
        "        「RR 不解决幻读」的说法不同 —— 教科书讲的是 SQL 标准，\n"
        "        InnoDB 的实现比标准更强。\n"
        "      - 但有一个著名例外：先快照读，再在同一事务里对该行做当前读/更新，\n"
        "        会「看到」自己之前看不到的行。这是 RR 下唯一残留的幻读现象。\n"
        "\n"
        "  MySQL 默认 RR，而 PostgreSQL / Oracle / SQL Server 默认 RC。\n"
        "  为什么 MySQL 选 RR？历史原因：早期的 binlog 只有 STATEMENT 格式，\n"
        "  RC 下会导致主从数据不一致。现在用 ROW 格式已无此问题，\n"
        "  很多大厂（阿里等）的规范里明确要求改成 RC —— 因为 RC 的\n"
        "  锁范围更小、间隙锁更少、死锁概率更低。\n";
}

// =============================================================================
// 17.9  MVCC —— 手写版本链 + ReadView
//
// MVCC (Multi-Version Concurrency Control) 让「读」不加锁：
// 读操作去找一个「对我可见的历史版本」，而不是等写锁释放。
// 于是**读写不互相阻塞**，这是 InnoDB 高并发的根本原因。
//
// 三个组成部分：
//   1. 每行的隐藏列 DB_TRX_ID（谁改的）和 DB_ROLL_PTR（上个版本在哪）
//   2. undo log 里串起来的版本链
//   3. ReadView —— 事务开始快照读时拍下的「当时谁在运行」的快照
//
// 版本链示意（一行被三个事务先后修改）：
//
//   当前行 (trx_id=30) ──roll_ptr──► undo(trx_id=20) ──► undo(trx_id=10)
//   name='v3'                        name='v2'            name='v1'
//
// 可见性判断算法（对每个版本从新到旧依次判断）：
//
//   设版本的 trx_id 为 T，ReadView 有 4 个字段：
//     m_ids        创建 ReadView 时**仍活跃**（未提交）的事务 id 集合
//     min_trx_id   m_ids 中的最小值
//     max_trx_id   下一个将分配的事务 id
//     creator_trx_id  当前事务自己的 id
//
//   1. T == creator_trx_id      -> 可见（自己改的当然能看见）
//   2. T < min_trx_id           -> 可见（在我拍快照前就已提交）
//   3. T >= max_trx_id          -> 不可见（我拍快照之后才开启的事务）
//   4. min_trx_id <= T < max_trx_id：
//        T ∈ m_ids  -> 不可见（拍快照时它还没提交）
//        T ∉ m_ids  -> 可见（拍快照时它已提交）
//
//   不可见就顺 roll_ptr 往老版本走，直到找到可见的或链走完。
//
// RC 与 RR 的唯一区别就在**什么时候创建 ReadView**：
//   RC：每条 SELECT 语句都新建一个 -> 能看到别人新提交的 -> 不可重复读
//   RR：事务内第一条 SELECT 建一个，之后一直复用 -> 整个事务视图一致
// =============================================================================

namespace mysql_sim {

using TrxId = uint64_t;

// 一行的一个历史版本
struct RowVersion {
    TrxId       trxId;        // DB_TRX_ID：哪个事务产生了这个版本
    std::string value;
    bool        deleted = false;
    std::shared_ptr<RowVersion> next;   // DB_ROLL_PTR：指向更老的版本
};

struct ReadView {
    std::set<TrxId> activeIds;      // m_ids
    TrxId           minTrxId;
    TrxId           maxTrxId;
    TrxId           creatorTrxId;

    std::string describe() const {
        std::ostringstream ss;
        ss << "ReadView{creator=" << creatorTrxId << ", min=" << minTrxId
           << ", max=" << maxTrxId << ", active={";
        bool first = true;
        for (TrxId id : activeIds) { if (!first) ss << ","; ss << id; first = false; }
        ss << "}}";
        return ss.str();
    }

    // 上面注释里的四步判断，逐条对应
    bool isVisible(TrxId t, std::string* why = nullptr) const {
        if (t == creatorTrxId) {
            if (why) *why = "自己修改的";
            return true;
        }
        if (t < minTrxId) {
            if (why) *why = "早于快照且已提交";
            return true;
        }
        if (t >= maxTrxId) {
            if (why) *why = "快照之后才开启的事务";
            return false;
        }
        if (activeIds.count(t)) {
            if (why) *why = "快照时该事务仍未提交";
            return false;
        }
        if (why) *why = "快照时该事务已提交";
        return true;
    }
};

// 极简 MVCC 存储：一个主键 -> 版本链头
class MvccStore {
    std::unordered_map<int, std::shared_ptr<RowVersion>> rows_;
    TrxId                                               nextTrxId_ = 1;
    std::set<TrxId>                                     activeTrx_;

public:
    TrxId beginTransaction() {
        TrxId id = nextTrxId_++;
        activeTrx_.insert(id);
        return id;
    }
    void commit(TrxId id) { activeTrx_.erase(id); }

    ReadView makeReadView(TrxId creator) const {
        ReadView rv;
        rv.activeIds    = activeTrx_;
        rv.activeIds.erase(creator);              // 自己不算「活跃的别人」
        rv.minTrxId     = rv.activeIds.empty() ? nextTrxId_ : *rv.activeIds.begin();
        rv.maxTrxId     = nextTrxId_;
        rv.creatorTrxId = creator;
        return rv;
    }

    // 写入：新版本挂到链头，旧版本成为它的 next（这就是 undo log 的作用）
    void write(int key, const std::string& value, TrxId trx) {
        auto v = std::make_shared<RowVersion>();
        v->trxId = trx;
        v->value = value;
        auto it = rows_.find(key);
        v->next = (it != rows_.end()) ? it->second : nullptr;
        rows_[key] = v;
    }

    void remove(int key, TrxId trx) {
        auto v = std::make_shared<RowVersion>();
        v->trxId   = trx;
        v->deleted = true;                        // 删除也是一个版本（标记删除）
        auto it = rows_.find(key);
        v->next = (it != rows_.end()) ? it->second : nullptr;
        rows_[key] = v;
    }

    // 快照读：沿版本链找第一个可见版本
    std::optional<std::string> read(int key, const ReadView& rv, bool verbose = false) const {
        auto it = rows_.find(key);
        if (it == rows_.end()) return std::nullopt;

        int step = 0;
        for (auto v = it->second; v; v = v->next) {
            std::string why;
            bool vis = rv.isVisible(v->trxId, &why);
            if (verbose) {
                std::cout << "        版本 " << ++step << ": trx_id=" << v->trxId
                          << " value=\"" << (v->deleted ? "<已删除>" : v->value) << "\""
                          << "  -> " << (vis ? "可见" : "不可见") << "（" << why << "）\n";
            }
            if (vis) {
                return v->deleted ? std::nullopt : std::optional<std::string>(v->value);
            }
        }
        if (verbose) std::cout << "        版本链走完，无可见版本\n";
        return std::nullopt;
    }

    int chainLength(int key) const {
        auto it = rows_.find(key);
        int n = 0;
        for (auto v = (it == rows_.end() ? nullptr : it->second); v; v = v->next) ++n;
        return n;
    }
};

} // namespace mysql_sim

static void s09_mvcc() {
    demo::title("17.9  MVCC 版本链与 ReadView");

    demo::section("场景：三个事务交错读写同一行");
    {
        using namespace mysql_sim;
        MvccStore store;

        // 事务 1 写入初始值并提交
        TrxId t1 = store.beginTransaction();
        store.write(100, "v1-初始值", t1);
        store.commit(t1);
        std::cout << "  事务 " << t1 << ": 写入 \"v1-初始值\" 并提交\n";

        // 事务 2 开启（长事务，先不提交）
        TrxId t2 = store.beginTransaction();
        std::cout << "  事务 " << t2 << ": 开启（暂不提交）\n";

        // 事务 3 修改并提交
        TrxId t3 = store.beginTransaction();
        store.write(100, "v2-被事务3修改", t3);
        store.commit(t3);
        std::cout << "  事务 " << t3 << ": 改成 \"v2-被事务3修改\" 并提交\n";

        // 事务 4 修改但**不**提交
        TrxId t4 = store.beginTransaction();
        store.write(100, "v3-事务4未提交", t4);
        std::cout << "  事务 " << t4 << ": 改成 \"v3-事务4未提交\"（未提交）\n";

        std::cout << "\n  当前版本链长度 = " << store.chainLength(100) << "\n";

        // ---- 事务 2 在 RR 下读 ----
        std::cout << "\n  【事务 " << t2 << " 做快照读（RR：ReadView 在事务开始时确定）】\n";
        // RR 模拟：ReadView 应该在 t2 的第一条 SELECT 时创建。
        // 这里为演示效果，手工构造 t2 开始时刻的 ReadView：
        // 那时 t3、t4 还不存在，活跃集合为空，max_trx_id = 3
        ReadView rvRR;
        rvRR.activeIds    = {};
        rvRR.minTrxId     = 3;
        rvRR.maxTrxId     = 3;      // t3(=3) 及之后的都不可见
        rvRR.creatorTrxId = t2;
        std::cout << "      " << rvRR.describe() << "\n";
        auto r1 = store.read(100, rvRR, true);
        std::cout << "      结果: " << (r1 ? *r1 : "NULL") << "\n";
        std::cout << "      ^^^ 看到 v1。事务 3 虽然已提交，但它在 t2 的快照之后 -> 不可见。\n";

        // ---- 同一时刻，一个新事务在 RC 下读 ----
        std::cout << "\n  【新事务做快照读（RC：每条语句都新建 ReadView）】\n";
        TrxId t5 = store.beginTransaction();
        ReadView rvRC = store.makeReadView(t5);
        std::cout << "      " << rvRC.describe() << "\n";
        auto r2 = store.read(100, rvRC, true);
        std::cout << "      结果: " << (r2 ? *r2 : "NULL") << "\n";
        std::cout << "      ^^^ 看到 v2（事务 3 已提交），但看不到 v3（事务 4 未提交）。\n";
        std::cout << "          这正是「已提交读」的语义。\n";

        // ---- 事务 4 读自己的修改 ----
        std::cout << "\n  【事务 " << t4 << " 读自己的未提交修改】\n";
        ReadView rvSelf = store.makeReadView(t4);
        auto r3 = store.read(100, rvSelf, true);
        std::cout << "      结果: " << (r3 ? *r3 : "NULL") << "\n";
        std::cout << "      ^^^ 自己改的自己一定能看见（判断规则第 1 条）。\n";
    }

    demo::section("MVCC 的代价：长事务会撑大 undo 表空间");
    std::cout <<
        "  只要还有事务的 ReadView 可能需要某个老版本，undo log 就不能清理。\n"
        "  一个开着不提交的长事务（比如忘了 commit 的会话），\n"
        "  会导致 undo 一直累积 —— 这是生产环境 ibdata 暴涨的经典原因。\n"
        "\n"
        "  排查：\n"
        "    SELECT trx_id, trx_started,\n"
        "           TIMESTAMPDIFF(SECOND, trx_started, NOW()) AS duration_s,\n"
        "           trx_rows_modified, trx_query\n"
        "    FROM information_schema.innodb_trx\n"
        "    ORDER BY trx_started;\n"
        "\n"
        "  预防：\n"
        "    - 设 innodb_max_undo_log_size 并开启 innodb_undo_log_truncate\n"
        "    - 应用侧设事务超时，别把事务跨越网络调用或用户交互\n"
        "    - 监控 information_schema.innodb_trx 里的长事务并告警\n";
}

// =============================================================================
// 17.10  锁
// =============================================================================
static void s10_locks() {
    demo::title("17.10  锁：行锁 / 间隙锁 / next-key lock");

    std::cout <<
        "  InnoDB 的锁**加在索引上**，不是加在行上 —— 这是理解所有锁问题的前提。\n"
        "  推论：如果 WHERE 条件用不到索引，就只能锁住扫过的**所有**记录，\n"
        "        效果近似锁表。所以「索引失效」不仅慢，还会放大锁冲突。\n"
        "\n"
        "  三种行级锁：\n"
        "\n"
        "    Record Lock  记录锁     锁住索引上的一条具体记录\n"
        "    Gap Lock     间隙锁     锁住两条记录**之间**的空隙，\n"
        "                            阻止别人往这个空隙里插入 -> 防幻读\n"
        "    Next-Key     临键锁     Record Lock + 前面的 Gap Lock，\n"
        "                            即左开右闭区间 (前一条, 本条]\n"
        "                            **RR 隔离级别下的默认加锁方式**\n"
        "\n"
        "  例：表里有 id = 5, 10, 15, 20\n"
        "  间隙划分：(-∞,5)  (5,10)  (10,15)  (15,20)  (20,+∞)\n"
        "\n"
        "    SELECT * FROM t WHERE id = 10 FOR UPDATE;\n"
        "      -> id 是主键（唯一索引）且记录存在，next-key 退化成 Record Lock，\n"
        "         只锁 id=10 这一行。别人插 id=12 不受影响。\n"
        "\n"
        "    SELECT * FROM t WHERE id = 12 FOR UPDATE;   -- 记录不存在\n"
        "      -> 锁住间隙 (10,15)。别人插 id=11/12/13/14 全部被阻塞。\n"
        "         这就是「查不存在的行也会加锁」的原因，\n"
        "         也是很多插入死锁的源头。\n"
        "\n"
        "    SELECT * FROM t WHERE id > 10 AND id <= 18 FOR UPDATE;\n"
        "      -> 锁 (10,15] 和 (15,20]。注意右边界扩到了 20！\n"
        "         因为 next-key 是左开右闭，18 落在 (15,20] 里，整个区间被锁。\n"
        "\n"
        "  两种表级意向锁（用于快速判断「表里有没有行锁」）：\n"
        "    IS  意向共享锁     准备加行级 S 锁前先在表上加\n"
        "    IX  意向排他锁     准备加行级 X 锁前先在表上加\n"
        "    作用：想加表锁时，只需检查有无 IS/IX，不必逐行扫描。\n"
        "\n"
        "  插入意向锁 (Insert Intention Lock)：\n"
        "    INSERT 时加在间隙上的一种特殊锁。多个事务往**同一间隙的不同位置**\n"
        "    插入是兼容的（不冲突），但会被已存在的 Gap Lock 阻塞。\n";

    demo::section("死锁的经典成因与排查");
    std::cout <<
        "  成因 1：加锁顺序相反（与第 13 章坑 32 同源）\n"
        "    事务A: UPDATE t SET .. WHERE id=1;   然后  WHERE id=2;\n"
        "    事务B: UPDATE t SET .. WHERE id=2;   然后  WHERE id=1;\n"
        "    -> 互等，死锁\n"
        "    对策：让所有事务按**固定顺序**（比如 id 升序）访问行。\n"
        "          批量更新前先 ORDER BY id。\n"
        "\n"
        "  成因 2：唯一索引冲突 + 间隙锁\n"
        "    两个事务同时 INSERT 同一个唯一键：\n"
        "    先来的持有 X 锁，后来的检测到重复要加 S 锁等待，\n"
        "    此时先来的回滚 -> 两个都在等对方释放间隙 -> 死锁。\n"
        "    对策：改用 INSERT ... ON DUPLICATE KEY UPDATE，\n"
        "          或先 SELECT 判断再插（配合唯一索引兜底）。\n"
        "\n"
        "  成因 3：锁升级路径不同\n"
        "    先快照读再当前读，与直接当前读的加锁范围不同，交错时容易死锁。\n"
        "\n"
        "  排查步骤：\n"
        "    1. SHOW ENGINE INNODB STATUS;\n"
        "       看 \"LATEST DETECTED DEADLOCK\" 段，它会明确列出：\n"
        "         - 两个事务分别持有什么锁、在等什么锁\n"
        "         - 各自执行的 SQL\n"
        "         - InnoDB 回滚了哪一个（选 undo 量小的那个回滚）\n"
        "    2. 开 innodb_print_all_deadlocks=ON，把所有死锁记进错误日志\n"
        "       （默认只保留最近一次，不开这个选项会漏掉历史死锁）\n"
        "    3. innodb_lock_wait_timeout 默认 50 秒，通常应调小到 5~10 秒，\n"
        "       让失败快速暴露而不是拖死连接池。\n"
        "\n"
        "  重要认知：**死锁不可能完全避免**，应用层必须有重试逻辑。\n"
        "  InnoDB 会自动检测并回滚其中一个事务（报 1213 Deadlock found），\n"
        "  你的代码应该捕获它并重试整个事务（注意要重试，不是重试单条 SQL）。\n";
}

// =============================================================================
// 17.11  日志系统
// =============================================================================
static void s11_logs() {
    demo::title("17.11  redo log / undo log / binlog");

    std::cout <<
        "  三种日志，职责完全不同，别混：\n"
        "\n"
        "  ┌──────────┬────────────┬──────────────┬────────────────────────┐\n"
        "  │          │ 谁产生的   │ 记录什么     │ 用途                   │\n"
        "  ├──────────┼────────────┼──────────────┼────────────────────────┤\n"
        "  │ redo log │ InnoDB 引擎│ 物理：某页某 │ 崩溃恢复（持久性 D）   │\n"
        "  │          │            │ 偏移改成什么 │ 循环写，固定大小       │\n"
        "  ├──────────┼────────────┼──────────────┼────────────────────────┤\n"
        "  │ undo log │ InnoDB 引擎│ 逻辑：反向   │ 回滚（原子性 A）       │\n"
        "  │          │            │ 操作         │ + MVCC 版本链          │\n"
        "  ├──────────┼────────────┼──────────────┼────────────────────────┤\n"
        "  │ binlog   │ MySQL Server│ 逻辑：SQL 或│ 主从复制、时间点恢复   │\n"
        "  │          │（引擎无关） │ 行变更      │ 追加写，可归档         │\n"
        "  └──────────┴────────────┴──────────────┴────────────────────────┘\n"
        "\n"
        "  【WAL：Write-Ahead Logging】\n"
        "    修改数据时**先写日志，再改数据页**。\n"
        "    为什么这样更快？\n"
        "      改数据页是**随机**写（页散布在磁盘各处）\n"
        "      写 redo log 是**顺序**追加\n"
        "      顺序写比随机写快 1~2 个数量级。\n"
        "    于是：先顺序写日志保证不丢，脏页慢慢在后台刷。\n"
        "    崩溃了就重放 redo log 把页恢复出来。\n"
        "\n"
        "  【两阶段提交 (2PC)】—— 为什么需要它\n"
        "    redo log 是引擎层的，binlog 是 Server 层的，两者必须一致。\n"
        "    否则主从数据会不一致：\n"
        "\n"
        "      假设只写 redo 不写 binlog 就崩溃：\n"
        "        主库恢复后有这条数据，从库靠 binlog 复制 -> 没有 -> 不一致\n"
        "      假设只写 binlog 不写 redo 就崩溃：\n"
        "        主库恢复后没有，从库有 -> 不一致\n"
        "\n"
        "    所以提交流程是：\n"
        "      1. 写 redo log，标记为 **prepare** 状态\n"
        "      2. 写 binlog 并 fsync\n"
        "      3. 写 redo log，标记为 **commit** 状态\n"
        "\n"
        "    崩溃恢复时的判定规则：\n"
        "      redo 是 commit           -> 直接提交\n"
        "      redo 是 prepare 且 binlog 完整 -> 提交（因为从库会执行它）\n"
        "      redo 是 prepare 且 binlog 不完整 -> 回滚\n"
        "\n"
        "  【两个关键参数（数据安全 vs 性能的核心权衡）】\n"
        "    innodb_flush_log_at_trx_commit\n"
        "      1  每次提交都 fsync redo log      最安全，默认。掉电不丢数据\n"
        "      2  每次写 OS 缓存，每秒 fsync     MySQL 进程崩溃不丢，掉电丢 1 秒\n"
        "      0  每秒才写并 fsync               最快，崩溃就丢 1 秒\n"
        "\n"
        "    sync_binlog\n"
        "      1  每次提交都 fsync binlog        最安全，默认\n"
        "      0  交给操作系统决定               最快\n"
        "      N  每 N 次提交 fsync 一次         折中\n"
        "\n"
        "    「双 1」配置（=1, =1）是金融级标准，性能损失明显；\n"
        "    很多互联网业务用 (2, 1000) 换吞吐，接受掉电丢 1 秒的风险。\n"
        "    这是个**业务决策**，不是技术最优解问题。\n"
        "\n"
        "  【binlog 三种格式】\n"
        "    STATEMENT  记 SQL 原文。日志小，但 NOW()、UUID()、\n"
        "               LIMIT 无 ORDER BY 等不确定语句会导致主从不一致\n"
        "    ROW        记每一行的前后镜像。安全，是**现在的推荐值**，\n"
        "               缺点是一条 UPDATE 影响百万行就产生百万条记录\n"
        "    MIXED      平时 STATEMENT，遇到不确定语句自动切 ROW\n";
}

// =============================================================================
// 17.12  Buffer Pool 与改进版 LRU
//
// Buffer Pool 是 InnoDB 在内存里缓存数据页的地方，通常配置为物理内存的 50~75%。
// 它是 MySQL **最重要**的性能参数：命中率从 95% 掉到 90%，
// 磁盘 IO 就翻倍。
//
// 【为什么不能用朴素 LRU？】
//
//   问题 1：预读失效
//     InnoDB 会「线性预读」——访问一个区里的多个页时，把整个区(64 页)读进来。
//     如果预读的页最终没被访问，它们却占在 LRU 头部，
//     把真正的热数据挤到尾部去了。
//
//   问题 2：缓冲池污染（更严重）
//     一条 SELECT * FROM big_table 全表扫描，读进几百万个页，
//     每个页只用一次。朴素 LRU 会把**所有热数据全部淘汰**，
//     换成这些一次性的页。扫描结束后，缓存命中率归零，
//     线上响应时间瞬间飙升。
//
// 【InnoDB 的解法：LRU 链表分成 young 区和 old 区】
//
//   ┌──────────────── young (默认 5/8) ────────────┬──── old (3/8) ────┐
//   │ 热数据                                       │ 新读入的页在这里  │
//   └──────────────────────────────────────────────┴───────────────────┘
//     ↑ head                                          midpoint      tail ↑
//                                                                   淘汰端
//
//   规则 1：新页插入到 **midpoint**（old 区头部），不是链表头。
//           -> 全表扫描的页只在 old 区打转，动不到 young 区的热数据。
//
//   规则 2：old 区的页被再次访问时，只有距首次访问超过
//           innodb_old_blocks_time（默认 1000 ms）才提升到 young 区头部。
//           -> 全表扫描时同一页在短时间内被连续访问（一页有多行），
//              但间隔 < 1 秒，所以**不会**被提升。
//
//   这两条规则组合起来，让全表扫描几乎不影响热数据缓存。
// =============================================================================

namespace mysql_sim {

class BufferPool {
public:
    explicit BufferPool(size_t capacity, double oldRatio = 3.0 / 8.0)
        : capacity_(capacity), oldRatio_(oldRatio) {}

    // 朴素 LRU：新页永远插链表头
    struct NaiveLru {
        size_t                                    cap;
        std::list<int>                            lru;      // 头 = 最近使用
        std::unordered_map<int, std::list<int>::iterator> pos;
        long long hits = 0, misses = 0;

        explicit NaiveLru(size_t c) : cap(c) {}

        void access(int page) {
            auto it = pos.find(page);
            if (it != pos.end()) {
                ++hits;
                lru.splice(lru.begin(), lru, it->second);   // 移到头部
                return;
            }
            ++misses;
            if (lru.size() >= cap) {                        // 淘汰尾部
                pos.erase(lru.back());
                lru.pop_back();
            }
            lru.push_front(page);
            pos[page] = lru.begin();
        }
        bool contains(int page) const { return pos.count(page) != 0; }
    };

    // InnoDB 式：young / old 分区
    struct InnodbLru {
        size_t         cap;
        double         oldRatio;    // old 区占链表长度的比例（InnoDB 默认 0.375）
        std::list<int> lru;
        struct Entry {
            std::list<int>::iterator it;
            long long firstAccessMs;   // 首次进入 old 区的时间
            bool      inYoung;
        };
        std::unordered_map<int, Entry> pos;
        long long hits = 0, misses = 0, promotions = 0;
        long long clock = 0;         // 模拟时间（毫秒）
        long long oldBlocksTime = 1000;

        InnodbLru(size_t c, double ratio) : cap(c), oldRatio(ratio) {}

        // midpoint 迭代器：young 区末尾 / old 区开头
        //
        // 注意 old 区是当前链表长度的**比例**（默认 3/8），不是固定条数。
        // 写成固定条数会有个隐蔽的错误：缓冲池还没填满时（比如只装了 40 页），
        // 固定 37 条的 old 区会占掉几乎整个链表，midpoint 落到位置 3，
        // 新页于是被插进热数据中间 —— 防污染效果完全失效。
        // 真实的 innodb_old_blocks_pct 也是百分比，就是这个道理。
        std::list<int>::iterator midpoint() {
            size_t youngLen = static_cast<size_t>(
                static_cast<double>(lru.size()) * (1.0 - oldRatio));
            auto   it = lru.begin();
            std::advance(it, static_cast<long>(youngLen));
            return it;
        }

        void access(int page, long long timeAdvanceMs = 1) {
            clock += timeAdvanceMs;

            auto found = pos.find(page);
            if (found != pos.end()) {
                ++hits;
                Entry& e = found->second;
                if (e.inYoung) {
                    lru.splice(lru.begin(), lru, e.it);      // young 内部：提到最前
                } else {
                    // 规则 2：在 old 区待够时间才提升
                    if (clock - e.firstAccessMs >= oldBlocksTime) {
                        lru.splice(lru.begin(), lru, e.it);
                        e.inYoung = true;
                        ++promotions;
                    }
                    // 否则原地不动 —— 全表扫描就被挡在这里
                }
                return;
            }

            ++misses;
            if (lru.size() >= cap) {
                pos.erase(lru.back());
                lru.pop_back();
            }
            // 规则 1：插到 midpoint，而不是链表头
            auto mid = midpoint();
            auto it  = lru.insert(mid, page);
            pos[page] = Entry{it, clock, false};
        }
        bool contains(int page) const { return pos.count(page) != 0; }
    };

    size_t capacity_;
    double oldRatio_;
};

} // namespace mysql_sim

static void s12_buffer_pool() {
    demo::title("17.12  Buffer Pool 与改进版 LRU");

    demo::section("实验：热数据被全表扫描冲刷");
    {
        using namespace mysql_sim;
        constexpr size_t kPoolSize  = 100;    // 缓冲池只能放 100 页
        constexpr int    kHotPages  = 30;     // 30 个热页（页号 0..29）
        constexpr int    kWarmPages = 300;    // 预热用的杂项页
        constexpr int    kScanPages = 5000;   // 全表扫描 5000 个冷页

        BufferPool::NaiveLru  naive(kPoolSize);
        BufferPool::InnodbLru innodb(kPoolSize, 3.0 / 8.0);

        // 阶段 0：先用杂项页把缓冲池填满。
        //   这一步不能省 —— 空池和满池的行为完全不同。
        //   池没填满时 young 区还很短，装不下整个热点集合。
        //   生产环境的缓冲池永远是满的，所以要先还原这个前提。
        for (int p = 1000000; p < 1000000 + kWarmPages; ++p) {
            naive.access(p);
            innodb.access(p, 1);
        }

        // 阶段 1：正常业务，反复访问热页。
        //   注意访问间隔设为 100 ms：同一个热页两次访问相隔
        //   30 × 100 = 3000 ms > innodb_old_blocks_time(1000 ms)，
        //   所以它们会被提升进 young 区。
        for (int round = 0; round < 40; ++round) {
            for (int p = 0; p < kHotPages; ++p) {
                naive.access(p);
                innodb.access(p, 100);
            }
        }
        auto countHot = [&](auto& lru) {
            int n = 0;
            for (int p = 0; p < kHotPages; ++p) if (lru.contains(p)) ++n;
            return n;
        };
        std::cout << "  阶段 1（池已填满 + 建立热点后）缓存中的热页数：\n";
        std::cout << "    朴素 LRU   " << countHot(naive)  << "/" << kHotPages << "\n";
        std::cout << "    InnoDB LRU " << countHot(innodb) << "/" << kHotPages
                  << "   （其中 " << innodb.promotions << " 次 old -> young 提升）\n";

        // 阶段 2：一条 SELECT * FROM big_table。
        //   全表扫描的特征：页号连续、每页短时间内被访问几次（一页多行）、
        //   之后再也不用。间隔 1 ms 远小于 old_blocks_time，所以不该被提升。
        for (int p = 2000000; p < 2000000 + kScanPages; ++p) {
            naive.access(p);
            innodb.access(p, 1);
            innodb.access(p, 1);        // 同一页读多行
        }

        std::cout << "\n  阶段 2（全表扫描 " << kScanPages << " 页后）剩余热页数：\n";
        std::cout << "    朴素 LRU   " << countHot(naive) << "/" << kHotPages
                  << "   <== 热数据被冲光\n";
        std::cout << "    InnoDB LRU " << countHot(innodb) << "/" << kHotPages
                  << "   <== 全部保住（扫描页只在 old 区打转）\n";
        std::cout << "    扫描期间 InnoDB 的额外提升次数 = "
                  << innodb.promotions - kHotPages << "\n";

        // 阶段 3：业务恢复，对比命中率
        long long nh0 = naive.hits,  nm0 = naive.misses;
        long long ih0 = innodb.hits, im0 = innodb.misses;
        for (int round = 0; round < 20; ++round) {
            for (int p = 0; p < kHotPages; ++p) {
                naive.access(p);
                innodb.access(p, 100);
            }
        }
        auto rate = [](long long h, long long m) {
            long long t = h + m;
            return t ? (100.0 * static_cast<double>(h) / static_cast<double>(t)) : 0.0;
        };
        std::cout << "\n  阶段 3（业务恢复后的命中率）：\n";
        std::cout << "    朴素 LRU   " << std::fixed << std::setprecision(1)
                  << rate(naive.hits - nh0, naive.misses - nm0) << "%\n";
        std::cout << "    InnoDB LRU " << rate(innodb.hits - ih0, innodb.misses - im0)
                  << "%\n";
        std::cout <<
            "\n  朴素 LRU 的热数据要重新从磁盘加载一遍才能恢复；\n"
            "  在真实系统里这就是「跑了一条没加索引的大查询，\n"
            "  之后几分钟所有接口都变慢」的直接原因。\n";
    }

    demo::section("相关参数与监控");
    std::cout <<
        "  innodb_buffer_pool_size          最重要的参数，物理内存的 50~75%\n"
        "  innodb_buffer_pool_instances     分成多个实例减少内部锁竞争\n"
        "                                   （池 > 1 GB 时建议设为 8）\n"
        "  innodb_old_blocks_pct            old 区占比，默认 37（即 3/8）\n"
        "  innodb_old_blocks_time           默认 1000 ms，防污染的关键\n"
        "\n"
        "  命中率计算：\n"
        "    SHOW STATUS LIKE 'Innodb_buffer_pool_read%';\n"
        "    命中率 = 1 - Innodb_buffer_pool_reads / Innodb_buffer_pool_read_requests\n"
        "    （reads = 从磁盘读的次数，read_requests = 逻辑读请求总数）\n"
        "    生产环境应 > 99%。低于 95% 就该考虑加内存或优化 SQL 了。\n"
        "\n"
        "  详细状态：SELECT * FROM information_schema.innodb_buffer_pool_stats;\n";
}

// =============================================================================
// 17.13  EXPLAIN 与优化流程
// =============================================================================
static void s13_explain() {
    demo::title("17.13  EXPLAIN 解读与慢查询优化");

    std::cout <<
        "  EXPLAIN SELECT ... 输出的关键列：\n"
        "\n"
        "  【type】访问类型，**从好到坏**排列，这是最该先看的列：\n"
        "    system    表只有一行（系统表）\n"
        "    const     通过主键或唯一索引等值匹配，最多一行。最快\n"
        "    eq_ref    JOIN 时用主键/唯一索引匹配，每次最多一行\n"
        "    ref       用非唯一索引等值匹配，返回多行\n"
        "    range     索引范围扫描（BETWEEN / > / IN）\n"
        "    index     扫描整棵索引树（比 ALL 好，因为索引比数据小）\n"
        "    ALL       全表扫描 ← **看到这个就要警觉**\n"
        "\n"
        "    实践底线：线上查询至少要到 range，理想是 ref 或更好。\n"
        "    例外：小表（几百行）全表扫反而更快，不必强行加索引。\n"
        "\n"
        "  【key】实际使用的索引。为 NULL 说明没用上索引。\n"
        "  【possible_keys】优化器**考虑过**的索引。\n"
        "     如果 possible_keys 有值但 key 是 NULL，说明优化器主动放弃了\n"
        "     —— 通常是选择性太差，或统计信息过期（试 ANALYZE TABLE）。\n"
        "\n"
        "  【rows】预估要检查的行数。注意是**估算值**，不精确。\n"
        "     和最终返回行数差距巨大时，说明过滤效率低。\n"
        "\n"
        "  【filtered】按条件过滤后剩下的百分比。\n"
        "     rows × filtered / 100 ≈ 实际参与后续操作的行数。\n"
        "\n"
        "  【key_len】使用了索引的前多少字节。用它判断联合索引用到了几列！\n"
        "     计算规则：INT=4，BIGINT=8，可为 NULL 的列 +1，\n"
        "               VARCHAR(n) utf8mb4 = 4n + 2（长度前缀）\n"
        "     例：KEY idx(a INT NOT NULL, b INT NOT NULL)\n"
        "         key_len=4 -> 只用到 a；key_len=8 -> a 和 b 都用到了\n"
        "\n"
        "  【Extra】最信息量大的一列：\n"
        "    Using index               覆盖索引，不回表。**好**\n"
        "    Using where               用 WHERE 过滤了从表里取出的行\n"
        "    Using index condition     索引条件下推 (ICP)，在引擎层就过滤，好\n"
        "    Using filesort            需要额外排序。**要优化**\n"
        "                              （不一定用磁盘，内存排序也叫这个名字）\n"
        "    Using temporary           用了临时表，常见于 GROUP BY / DISTINCT。\n"
        "                              **要优化**\n"
        "    Using join buffer         JOIN 没用上索引，退化成 BNL 算法。要优化\n"
        "    Impossible WHERE          条件恒假，不会执行\n"
        "    Select tables optimized away   聚合被优化掉了（如 MyISAM 的 COUNT(*)）\n"
        "\n"
        "  进阶用法：\n"
        "    EXPLAIN FORMAT=JSON SELECT ...    带 cost 估算，能看到优化器的账本\n"
        "    EXPLAIN ANALYZE SELECT ...        8.0.18+，**真正执行**并给出实际耗时，\n"
        "                                      比 EXPLAIN 的估算可信得多\n";

    demo::section("慢查询优化的标准流程");
    std::cout <<
        "  1. 定位\n"
        "     开慢查询日志 -> pt-query-digest 分析 -> 找出总耗时占比最高的语句。\n"
        "     注意排序依据是「总耗时」而不是「单次耗时」：\n"
        "     一条 10 ms 但每秒执行 1000 次的 SQL，比一条 5 秒但每天跑一次的更值得优化。\n"
        "\n"
        "  2. 分析\n"
        "     EXPLAIN 看 type / key / rows / Extra。\n"
        "     优先级：ALL -> 加索引；filesort/temporary -> 调索引顺序或改写。\n"
        "\n"
        "  3. 优化手段（按性价比排序）\n"
        "     a) 加/调索引         最常见，收益最大\n"
        "        - 让 WHERE 的等值列在联合索引前面，范围列放最后\n"
        "        - 让 ORDER BY 的列跟在等值列后面，可消除 filesort\n"
        "        - 把 SELECT 的列并入索引 -> 覆盖索引，消除回表\n"
        "     b) 改写 SQL\n"
        "        - SELECT * 改成只取需要的列（可能变成覆盖索引）\n"
        "        - 深分页改 keyset 分页（WHERE id > last_id）\n"
        "        - 大 IN 列表改 JOIN 临时表\n"
        "        - 子查询改 JOIN（MySQL 对某些子查询优化不好）\n"
        "        - 大批量 UPDATE/DELETE 拆成小批次（减少锁范围和主从延迟）\n"
        "     c) 调整表结构\n"
        "        - 拆冷热字段（大 TEXT 单独存）\n"
        "        - 适度反范式，用冗余换掉 JOIN\n"
        "     d) 加缓存 / 读写分离 / 分库分表\n"
        "        ← 最后才考虑。这些引入的复杂度远大于前三项\n"
        "\n"
        "  4. 验证\n"
        "     EXPLAIN ANALYZE 对比前后实际耗时。\n"
        "     别忘了看**写入**是否变慢了（每个索引都会拖慢 INSERT/UPDATE）。\n";

    demo::section("常见误区纠正");
    std::cout <<
        "  ✗ 「索引越多越好」\n"
        "    每个索引都要占空间、拖慢写入、增加优化器选择成本。\n"
        "    单表索引建议不超过 5~6 个。\n"
        "\n"
        "  ✗ 「COUNT(1) 比 COUNT(*) 快」\n"
        "    完全一样。InnoDB 对 COUNT(*) 有专门优化，会选最小的索引扫。\n"
        "    COUNT(列) 才不同 —— 它会跳过 NULL，语义都不一样。\n"
        "\n"
        "  ✗ 「主键就是聚簇索引，所以查主键最快」\n"
        "    对，但注意 SELECT * WHERE id IN (1000 个 id) 是 1000 次随机 IO，\n"
        "    可能比一次范围扫描慢。\n"
        "\n"
        "  ✗ 「JOIN 一定比子查询慢」/「子查询一定比 JOIN 慢」\n"
        "    取决于具体情况和 MySQL 版本。8.0 的优化器对半连接优化好了很多。\n"
        "    结论只能靠 EXPLAIN ANALYZE 实测。\n"
        "\n"
        "  ✗ 「加了索引就一定会用」\n"
        "    见 17.7 的 8 种失效场景，尤其是隐式类型转换。\n";
}

// =============================================================================
int main() {
    demo::title("第 17 章  MySQL：命令 + 实现原理");
    std::cout <<
        "  本章不需要安装 MySQL。原理部分用可运行的模拟实现：\n"
        "    手写 B+ 树        观察分裂、树高、范围查询的页访问次数\n"
        "    MVCC 版本链       亲手验证 ReadView 的可见性判断规则\n"
        "    Buffer Pool LRU   实测全表扫描如何冲刷热数据，以及 InnoDB 怎么防\n";

    s01_relational_model();
    s02_commands();
    s03_engines();
    s04_bplus_tree();
    s05_clustered_index();
    s06_page_format();
    s07_index_failure();
    s08_transactions();
    s09_mvcc();
    s10_locks();
    s11_logs();
    s12_buffer_pool();
    s13_explain();

    demo::title("本章要点");
    std::cout <<
        "   1. B+ 树：非叶子只存键 -> 扇出大 -> 树高 3 层存千万行 -> 1~3 次 IO\n"
        "      叶子链表让范围查询变成顺序读，这是它胜过 B 树的关键\n"
        "   2. InnoDB 表就是聚簇索引；二级索引叶子存主键 -> 回表；\n"
        "      覆盖索引消除回表。主键要短且自增\n"
        "   3. 页 16 KB 是 IO 最小单位；单行别超 8 KB\n"
        "   4. 最左前缀：联合索引按字典序排；范围列放最后\n"
        "   5. 索引失效头号隐蔽杀手是隐式类型转换（VARCHAR 列传数字）\n"
        "   6. ACID = undo(A) + redo(D) + 锁与 MVCC(I)\n"
        "   7. MVCC 让读不加锁：版本链 + ReadView 四步可见性判断\n"
        "      RC 与 RR 的唯一区别是 ReadView 何时创建\n"
        "   8. 锁加在**索引**上；RR 默认 next-key lock（左开右闭）防幻读\n"
        "      死锁无法避免，应用必须重试整个事务\n"
        "   9. WAL + 两阶段提交保证 redo 与 binlog 一致\n"
        "  10. Buffer Pool 的 young/old 分区专门防全表扫描污染缓存\n"
        "  11. 优化先看 EXPLAIN 的 type 和 Extra；\n"
        "      加索引 > 改写 SQL > 改表结构 > 加缓存分库\n";
    return 0;
}
