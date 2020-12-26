/* slabs memory allocation */
#ifndef SLABS_H
#define SLABS_H

// slab class   1: chunk size        96 perslab   10922
// slab class   2: chunk size       120 perslab    8738
// slab class   3: chunk size       152 perslab    6898
// slab class   4: chunk size       192 perslab    5461
// ...
// slab class  41: chunk size    771184 perslab       1
// slab class  42: chunk size   1048576 perslab       1

/** Init the subsystem. 1st argument is the limit on no. of bytes to allocate,
    0 if no limit. 2nd argument is the growth factor; each slab will use a chunk
    size equal to the previous slab's chunk size times this factor.
    3rd argument specifies if the slab allocator should allocate all memory
    up front (if true), or allocate memory in chunks as it is needed (if false)
*/
// 初始化 slab 子系统
//   limit: 内存上限，默认值为 64M
//   factor: chunk size 增长因子，默认值为 1.25
//   prealloc: 是否预先分配好 slab 内存结构
void slabs_init(const size_t limit, const double factor, const bool prealloc);


/**
 * Given object size, return id to use when allocating/freeing memory for object
 * 0 means error: can't store such a large object
 */
// 根据数据大小找到最接近的 slabclass 下标
// slabs_clsid 最小值为 1，0 表示错误
unsigned int slabs_clsid(const size_t size);

/** Allocate object of given length. 0 on error */ /*@null@*/
// 根据数据大小分配一个 item
void *slabs_alloc(const size_t size, unsigned int id);

/** Free previously allocated object */
// 释放 item（实际操作是回收到 freelist 上供复用）
void slabs_free(void *ptr, size_t size, unsigned int id);

/** Adjust the stats for memory requested */
// 调整已请求内存计数（覆盖旧的 item 时需要用到）
void slabs_adjust_mem_requested(unsigned int id, size_t old, size_t ntotal);

// slab 统计信息相关
/** Return a datum for stats in binary protocol */
bool get_stats(const char *stat_type, int nkey, ADD_STAT add_stats, void *c);

/** Fill buffer with stats */ /*@null@*/
void slabs_stats(ADD_STAT add_stats, void *c);

// slab 后台内存优化维护线程相关
int start_slab_maintenance_thread(void);
void stop_slab_maintenance_thread(void);

enum reassign_result_type {
    REASSIGN_OK=0, REASSIGN_RUNNING, REASSIGN_BADCLASS, REASSIGN_NOSPARE,
    REASSIGN_SRC_DST_SAME
};

enum reassign_result_type slabs_reassign(int src, int dst);

void slabs_rebalancer_pause(void);
void slabs_rebalancer_resume(void);

#endif
