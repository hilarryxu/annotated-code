#ifndef HASH_H
#define    HASH_H

typedef uint32_t (*hash_func)(const void *key, size_t length);
hash_func hash;

enum hashfunc_type {
    JENKINS_HASH=0, MURMUR3_HASH
};

// 设置使用哪种 hash 函数
//   JENKINS_HASH (默认)
//   MURMUR3_HASH
int hash_init(enum hashfunc_type type);

#endif    /* HASH_H */

