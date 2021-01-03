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
  raw
  int
  embstr
hash
  ziplist
  hashtable
list
  ziplist
  linkedlist
  quicklist
set
  intset
  hashtable
zset
  ziplist
  skiplist
```
