/*
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#ifndef __REDIS_RIO_H
#define __REDIS_RIO_H

#include <stdio.h>
#include <stdint.h>
#include "sds.h"


//---------------------------------------------------------------------
// rdb 文件操作
//---------------------------------------------------------------------
struct _rio {
    /* Backend functions.
     * Since this functions do not tolerate short writes or reads the return
     * value is simplified to: zero on error, non zero on complete success. */
    // 文件读写相关操作接口
    //
    // 返回 0 表示出错
    size_t (*read)(struct _rio *, void *buf, size_t len);
    size_t (*write)(struct _rio *, const void *buf, size_t len);
    off_t (*tell)(struct _rio *);
    int (*flush)(struct _rio *);

    /* The update_cksum method if not NULL is used to compute the checksum of
     * all the data that was read or written so far. The method should be
     * designed so that can be called with the current checksum, and the buf
     * and len fields pointing to the new block of data to add to the checksum
     * computation. */
    // 校验和计算函数
    void (*update_cksum)(struct _rio *, const void *buf, size_t len);

    /* The current checksum */
    // 64 位的检验和
    uint64_t cksum;

    /* number of bytes read or written */
    // 已处理字节数
    size_t processed_bytes;

    /* maximum single read or write chunk size */
    // 单次读写最大数据库长度
    size_t max_processing_chunk;

    /* Backend-specific vars. */
    // 具体实现相关结构
    //
    // 内存缓冲过去
    // 文件描述符
    // 多文件描述符
    union {
        /* In-memory buffer target. */
        struct {
            sds ptr;
            off_t pos;
        } buffer;

        /* Stdio file pointer target. */
        struct {
            // 文件指针
            FILE *fp;
            // 最近一次 fsync() 以来，写入的字节量
            off_t buffered; /* Bytes written since last fsync. */
            // 写入多少字节之后，才会自动执行一次 fsync()
            off_t autosync; /* fsync after 'autosync' bytes written. */
        } file;

        /* Multiple FDs target (used to write to N sockets). */
        struct {
            // tcp socket fds
            int *fds;       /* File descriptors. */
            int *state;     /* Error state of each fd. 0 (if ok) or errno. */
            int numfds;
            // 临时缓冲区
            off_t pos;
            sds buf;
        } fdset;
    } io;
};

typedef struct _rio rio;

/* The following functions are our interface with the stream. They'll call the
 * actual implementation of read / write / tell, and will update the checksum
 * if needed. */

//---------------------------------------------------------------------
// 写数据
//
// 成功返回 1，失败返回 0
//---------------------------------------------------------------------
static inline size_t rioWrite(rio *r, const void *buf, size_t len) {
    while (len) {
        // 限制单次写入最大数据量
        size_t bytes_to_write = (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
        // 更新校验和
        if (r->update_cksum) r->update_cksum(r,buf,bytes_to_write);
        // 执行写操作
        if (r->write(r,buf,bytes_to_write) == 0)
            return 0;
        buf = (char*)buf + bytes_to_write;
        len -= bytes_to_write;
        // 更新已处理字节数
        r->processed_bytes += bytes_to_write;
    }
    return 1;
}


//---------------------------------------------------------------------
// 读数据
//
// 成功返回 1，失败返回 0
//---------------------------------------------------------------------
static inline size_t rioRead(rio *r, void *buf, size_t len) {
    while (len) {
        // 限制单次读取最大数据量
        size_t bytes_to_read = (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
        // 执行读操作
        if (r->read(r,buf,bytes_to_read) == 0)
            return 0;
        // 更新校验和
        if (r->update_cksum) r->update_cksum(r,buf,bytes_to_read);
        buf = (char*)buf + bytes_to_read;
        len -= bytes_to_read;
        // 更新已处理字节数
        r->processed_bytes += bytes_to_read;
    }
    return 1;
}


//---------------------------------------------------------------------
// 返回当前偏移量
//---------------------------------------------------------------------
static inline off_t rioTell(rio *r) {
    return r->tell(r);
}


//---------------------------------------------------------------------
// flush 缓冲区
//---------------------------------------------------------------------
static inline int rioFlush(rio *r) {
    return r->flush(r);
}


//---------------------------------------------------------------------
// 3 中实现的初始化函数
//---------------------------------------------------------------------
void rioInitWithFile(rio *r, FILE *fp);
void rioInitWithBuffer(rio *r, sds s);
void rioInitWithFdset(rio *r, int *fds, int numfds);

size_t rioWriteBulkCount(rio *r, char prefix, int count);
size_t rioWriteBulkString(rio *r, const char *buf, size_t len);
size_t rioWriteBulkLongLong(rio *r, long long l);
size_t rioWriteBulkDouble(rio *r, double d);

void rioGenericUpdateChecksum(rio *r, const void *buf, size_t len);
void rioSetAutoSync(rio *r, off_t bytes);

#endif
