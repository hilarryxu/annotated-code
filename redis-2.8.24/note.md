# reids

## 特点

* 单线程的内存数据库（实现一些数据结构会简单很多）
* 丰富的功能
* RDB、AOF 持久化
* 主从复制
* 分布式集群功能


## 应用场景

* 缓存（带过期时间的 key）
* 排行榜（zset）
* 计数器（incr）
* 社交网站（set、zset）
* 消息队列（list）

## 重大版本

* 2.6
  * 支持 lua
* 2.8
  * 部分主从复制功能
  * 第二版 sentinel
* 3.0
  * cluster
* 3.2
  * geo
  * sds 优化
  * quicklist
  * lua debugger
  * 新的 RDB 格式
* 4.2
  * 模块系统
  * PSYNC 2.0
  * LFU
  * 非阻塞 del、flushdb
  * RDB-AOF 混合持久化格式

## 数据编码方式

```
string
  int
  embstr
  raw
list
  ziplist
  linkedlist
  quicklist
hash
  ziplist
  hashtable
set
  intset
  hashtable
zset
  ziplist
  skiplist
```

## sds

* 带长度的类 pascal 字符串
* 可以存放二进制数据
* 实际只读操作时可以当 `char*` 用
* 有一些而外空间开销，到使用比较方便

## dict

* 和 memcached 一样用两个散列表做渐进式 rehash（由于是单线程所以实现比它更简单）
* 限制桶大小为 2 的多少次幂，用 & 位操作取代取模操作，减小性能开销
* 渐进式 rehash 时，添加操作只发生在 ht[1] 中，保证 ht[0] 元素只减不增

## skiplist 跳跃表

有序的数据结构，效率可以和红黑树媲美，但是实现起来简单很多。
根据使用场景裁剪功能，可以做些特殊优化。

redis 中的实现相比一般跳跃表增加了：

* span 跨度（用于计算节点的 rank 值）
* backward 向前指针（用于从后往前遍历）

## ziplist 压缩列表

元素个数不多时的紧凑结构的数组，特点：

* 根据数据大小（整数位数、字符串长度）来合理安排内存结构
* 保存前一个节点长度（用于从后往前遍历）
* 采用内存紧凑的数组存储
* 通常连锁更新的节点数比较少（不会特别影响性能）
