/*
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

#include "redis.h"
#include <sys/uio.h>
#include <math.h>

static void setProtocolError(redisClient *c, int pos);

/* To evaluate the output buffer size of a client we need to get size of
 * allocated objects, however we can't used zmalloc_size() directly on sds
 * strings because of the trick they use to work (the header is before the
 * returned pointer), so we use this helper function. */

//---------------------------------------------------------------------
// 计算 sds 实际占用内存大小
//---------------------------------------------------------------------
size_t zmalloc_size_sds(sds s) {
    return zmalloc_size(s-sizeof(struct sdshdr));
}

void *dupClientReplyValue(void *o) {
    incrRefCount((robj*)o);
    return o;
}

int listMatchObjects(void *a, void *b) {
    return equalStringObjects(a,b);
}


//---------------------------------------------------------------------
// 创建客户端连接对象
//---------------------------------------------------------------------
redisClient *createClient(int fd) {
    redisClient *c = zmalloc(sizeof(redisClient));

    // fd = -1 表示是伪客户端
    /* passing -1 as fd it is possible to create a non connected client.
     * This is useful since all the Redis commands needs to be executed
     * in the context of a client. When commands are executed in other
     * contexts (for instance a Lua script) we need a non connected client. */
    if (fd != -1) {
        // 设置位非阻塞
        anetNonBlock(NULL,fd);
        // 设置 TcpNoDelay
        anetEnableTcpNoDelay(NULL,fd);
        // 判断是否设置 KeepAlive
        if (server.tcpkeepalive)
            anetKeepAlive(NULL,fd,server.tcpkeepalive);
        // 监听可读事件
        if (aeCreateFileEvent(server.el,fd,AE_READABLE,
            readQueryFromClient, c) == AE_ERR)
        {
            close(fd);
            zfree(c);
            return NULL;
        }
    }

    // 初始化其他属性
    // 设置 c->db
    selectDb(c,0);
    c->id = server.next_client_id++;
    // 套接字
    c->fd = fd;
    // 名字
    c->name = NULL;
    // 静态发送缓冲区偏移量
    c->bufpos = 0;
    // 初始化请求缓冲区为空
    c->querybuf = sdsempty();
    c->querybuf_peak = 0;
    // 命令请求类型
    c->reqtype = 0;
    // 命令参数数量
    c->argc = 0;
    c->argv = NULL;
    c->cmd = c->lastcmd = NULL;
    c->multibulklen = 0;
    c->bulklen = -1;
    // 已发送字节数
    c->sentlen = 0;
    // 状态标志
    c->flags = 0;
    // 创建时间和最后一次互动时间
    c->ctime = c->lastinteraction = server.unixtime;
    c->authenticated = 0;
    c->replstate = REDIS_REPL_NONE;
    c->repl_put_online_on_ack = 0;
    c->reploff = 0;
    c->repl_ack_off = 0;
    c->repl_ack_time = 0;
    c->slave_listening_port = 0;
    c->slave_capa = SLAVE_CAPA_NONE;
    // 响应缓冲区链表
    c->reply = listCreate();
    // 响应缓冲区链表中的数据总字节数
    c->reply_bytes = 0;
    c->obuf_soft_limit_reached_time = 0;
    listSetFreeMethod(c->reply,decrRefCountVoid);
    listSetDupMethod(c->reply,dupClientReplyValue);
    c->bpop.keys = dictCreate(&setDictType,NULL);
    c->bpop.timeout = 0;
    c->bpop.target = NULL;
    c->watched_keys = listCreate();
    c->pubsub_channels = dictCreate(&setDictType,NULL);
    c->pubsub_patterns = listCreate();
    c->peerid = NULL;
    listSetFreeMethod(c->pubsub_patterns,decrRefCountVoid);
    listSetMatchMethod(c->pubsub_patterns,listMatchObjects);

    // 普通客户端连接追加到 clients 链表尾部
    if (fd != -1) listAddNodeTail(server.clients,c);
    // 初始化 MULTI/EXEC 相关的属性
    initClientMultiState(c);

    // 返回 redisClient
    return c;
}

/* This function is called every time we are going to transmit new data
 * to the client. The behavior is the following:
 *
 * If the client should receive new data (normal clients will) the function
 * returns REDIS_OK, and make sure to install the write handler in our event
 * loop so that when the socket is writable new data gets written.
 *
 * If the client should not receive new data, because it is a fake client,
 * a master, a slave not yet online, or because the setup of the write handler
 * failed, the function returns REDIS_ERR.
 *
 * Typically gets called every time a reply is built, before adding more
 * data to the clients output buffers. If the function returns REDIS_ERR no
 * data should be appended to the output buffers. */

//---------------------------------------------------------------------
// 每次准备发送响应给客户端时都回调用该函数
//
// 只有该函数返回成功才会往发送缓冲区中添加数据
//---------------------------------------------------------------------
int prepareClientToWrite(redisClient *c) {
    // lua 伪客户端总是可以发送响应，直接返回
    if (c->flags & REDIS_LUA_CLIENT) return REDIS_OK;
    if ((c->flags & REDIS_MASTER) &&
        !(c->flags & REDIS_MASTER_FORCE_REPLY)) return REDIS_ERR;
    // 其他伪客户端返回错误
    if (c->fd <= 0) return REDIS_ERR; /* Fake client */
    // 发送缓冲区为空（即首次准备发送数据时）
    // 开始监听套接字可写事件准备发送数据
    if (c->bufpos == 0 && listLength(c->reply) == 0 &&
        (c->replstate == REDIS_REPL_NONE ||
         c->replstate == REDIS_REPL_ONLINE) && !c->repl_put_online_on_ack &&
        aeCreateFileEvent(server.el, c->fd, AE_WRITABLE,
        sendReplyToClient, c) == AE_ERR) return REDIS_ERR;
    return REDIS_OK;
}

/* Create a duplicate of the last object in the reply list when
 * it is not exclusively owned by the reply list. */

//---------------------------------------------------------------------
// 检测发送缓冲区链表尾部的字符串对象是否被其他地方引用
//
// 有其他地方引用
//   那就需要重新复制一份
// 无共享
//   可以直接复用修改，无需复制一份
//---------------------------------------------------------------------
robj *dupLastObjectIfNeeded(list *reply) {
    robj *new, *cur;
    listNode *ln;
    redisAssert(listLength(reply) > 0);
    ln = listLast(reply);
    cur = listNodeValue(ln);
    // 检测是否被其他地方引用
    if (cur->refcount > 1) {
        new = dupStringObject(cur);
        decrRefCount(cur);
        listNodeValue(ln) = new;
    }
    return listNodeValue(ln);
}

/* -----------------------------------------------------------------------------
 * Low level functions to add more data to output buffers.
 * -------------------------------------------------------------------------- */


//=====================================================================
// 发送响应相关函数
//=====================================================================

//---------------------------------------------------------------------
// 底层实现：添加至发送缓冲区
//---------------------------------------------------------------------
int _addReplyToBuffer(redisClient *c, char *s, size_t len) {
    // 计算剩余空间大小
    size_t available = sizeof(c->buf)-c->bufpos;

    // 检测标志 REDIS_CLOSE_AFTER_REPLY，无需响应直接返回
    if (c->flags & REDIS_CLOSE_AFTER_REPLY) return REDIS_OK;

    /* If there already are entries in the reply list, we cannot
     * add anything more to the static buffer. */
    // 缓冲区链表不为空时，往静态缓冲区中添加数据是错误的操作
    if (listLength(c->reply) > 0) return REDIS_ERR;

    /* Check that the buffer has enough space available for this string. */
    // 判断剩余空间是否足够
    if (len > available) return REDIS_ERR;

    // 拷贝数据并更新 c->bufpos 偏移量
    memcpy(c->buf+c->bufpos,s,len);
    c->bufpos+=len;
    return REDIS_OK;
}

void _addReplyObjectToList(redisClient *c, robj *o) {
    robj *tail;

    if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

    if (listLength(c->reply) == 0) {
        incrRefCount(o);
        listAddNodeTail(c->reply,o);
        c->reply_bytes += zmalloc_size_sds(o->ptr);
    } else {
        tail = listNodeValue(listLast(c->reply));

        /* Append to this object when possible. */
        if (tail->ptr != NULL &&
            sdslen(tail->ptr)+sdslen(o->ptr) <= REDIS_REPLY_CHUNK_BYTES)
        {
            c->reply_bytes -= zmalloc_size_sds(tail->ptr);
            tail = dupLastObjectIfNeeded(c->reply);
            tail->ptr = sdscatlen(tail->ptr,o->ptr,sdslen(o->ptr));
            c->reply_bytes += zmalloc_size_sds(tail->ptr);
        } else {
            incrRefCount(o);
            listAddNodeTail(c->reply,o);
            c->reply_bytes += zmalloc_size_sds(o->ptr);
        }
    }
    asyncCloseClientOnOutputBufferLimitReached(c);
}

/* This method takes responsibility over the sds. When it is no longer
 * needed it will be free'd, otherwise it ends up in a robj. */
void _addReplySdsToList(redisClient *c, sds s) {
    robj *tail;

    if (c->flags & REDIS_CLOSE_AFTER_REPLY) {
        sdsfree(s);
        return;
    }

    if (listLength(c->reply) == 0) {
        listAddNodeTail(c->reply,createObject(REDIS_STRING,s));
        c->reply_bytes += zmalloc_size_sds(s);
    } else {
        tail = listNodeValue(listLast(c->reply));

        /* Append to this object when possible. */
        if (tail->ptr != NULL &&
            sdslen(tail->ptr)+sdslen(s) <= REDIS_REPLY_CHUNK_BYTES)
        {
            c->reply_bytes -= zmalloc_size_sds(tail->ptr);
            tail = dupLastObjectIfNeeded(c->reply);
            tail->ptr = sdscatlen(tail->ptr,s,sdslen(s));
            c->reply_bytes += zmalloc_size_sds(tail->ptr);
            sdsfree(s);
        } else {
            listAddNodeTail(c->reply,createObject(REDIS_STRING,s));
            c->reply_bytes += zmalloc_size_sds(s);
        }
    }
    asyncCloseClientOnOutputBufferLimitReached(c);
}


//---------------------------------------------------------------------
// 添加数据到发送缓冲区链表中（静态发送缓冲区容量不够时）
//---------------------------------------------------------------------
void _addReplyStringToList(redisClient *c, char *s, size_t len) {
    robj *tail;

    // 无响应模式直接返回
    if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

    if (listLength(c->reply) == 0) {
        // 链表为空时
        // 新加一个字符串对象并追加到链表尾部
        robj *o = createStringObject(s,len);

        listAddNodeTail(c->reply,o);
        // 更新响应字节数
        c->reply_bytes += zmalloc_size_sds(o->ptr);
    } else {
        // 取出链表尾部的字符串对象
        tail = listNodeValue(listLast(c->reply));

        /* Append to this object when possible. */
        if (tail->ptr != NULL &&
            sdslen(tail->ptr)+len <= REDIS_REPLY_CHUNK_BYTES)
        {
            // 尝试合并到一起
            c->reply_bytes -= zmalloc_size_sds(tail->ptr);
            // 尝试复用尾部节点的字符串对象
            tail = dupLastObjectIfNeeded(c->reply);
            tail->ptr = sdscatlen(tail->ptr,s,len);
            c->reply_bytes += zmalloc_size_sds(tail->ptr);
        } else {
            // 数据太大，超过了 REDIS_REPLY_CHUNK_BYTES = 16KB
            // 就新建一个字符串对象并追加到链表尾部
            robj *o = createStringObject(s,len);

            listAddNodeTail(c->reply,o);
            c->reply_bytes += zmalloc_size_sds(o->ptr);
        }
    }
    asyncCloseClientOnOutputBufferLimitReached(c);
}

/* -----------------------------------------------------------------------------
 * Higher level functions to queue data on the client output buffer.
 * The following functions are the ones that commands implementations will call.
 * -------------------------------------------------------------------------- */

//---------------------------------------------------------------------
// 添加 robj 到发送缓冲区
//---------------------------------------------------------------------
void addReply(redisClient *c, robj *obj) {
    if (prepareClientToWrite(c) != REDIS_OK) return;

    /* This is an important place where we can avoid copy-on-write
     * when there is a saving child running, avoiding touching the
     * refcount field of the object if it's not needed.
     *
     * If the encoding is RAW and there is room in the static buffer
     * we'll be able to send the object to the client without
     * messing with its page. */
    if (obj->encoding == REDIS_ENCODING_RAW) {
        // 字符串
        if (_addReplyToBuffer(c,obj->ptr,sdslen(obj->ptr)) != REDIS_OK)
            _addReplyObjectToList(c,obj);
    } else if (obj->encoding == REDIS_ENCODING_INT) {
        /* Optimization: if there is room in the static buffer for 32 bytes
         * (more than the max chars a 64 bit integer can take as string) we
         * avoid decoding the object and go for the lower level approach. */
        // long long 整数

        // 判断静态发送缓冲区剩余空间是否可以容纳 long long 的字符串表示形式
        if (listLength(c->reply) == 0 && (sizeof(c->buf) - c->bufpos) >= 32) {
            char buf[32];
            int len;

            len = ll2string(buf,sizeof(buf),(long)obj->ptr);
            if (_addReplyToBuffer(c,buf,len) == REDIS_OK)
                return;
            /* else... continue with the normal code path, but should never
             * happen actually since we verified there is room. */
        }
        // 转成字符串对象
        obj = getDecodedObject(obj);
        if (_addReplyToBuffer(c,obj->ptr,sdslen(obj->ptr)) != REDIS_OK)
            _addReplyObjectToList(c,obj);
        decrRefCount(obj);
    } else {
        redisPanic("Wrong obj->encoding in addReply()");
    }
}

//---------------------------------------------------------------------
// 添加 sds 字符串到发送缓冲区
//---------------------------------------------------------------------
void addReplySds(redisClient *c, sds s) {
    if (prepareClientToWrite(c) != REDIS_OK) {
        /* The caller expects the sds to be free'd. */
        sdsfree(s);
        return;
    }
    if (_addReplyToBuffer(c,s,sdslen(s)) == REDIS_OK) {
        sdsfree(s);
    } else {
        /* This method free's the sds when it is no longer needed. */
        _addReplySdsToList(c,s);
    }
}


//---------------------------------------------------------------------
// 添加字符串到发送缓冲区
//---------------------------------------------------------------------
void addReplyString(redisClient *c, char *s, size_t len) {
    if (prepareClientToWrite(c) != REDIS_OK) return;
    if (_addReplyToBuffer(c,s,len) != REDIS_OK)
        _addReplyStringToList(c,s,len);
}


//---------------------------------------------------------------------
// 添加错误消息到发送缓冲区
//
// -ERR unknown command 'foobar'\r\n
//---------------------------------------------------------------------
void addReplyErrorLength(redisClient *c, char *s, size_t len) {
    addReplyString(c,"-ERR ",5);
    addReplyString(c,s,len);
    addReplyString(c,"\r\n",2);
}

void addReplyError(redisClient *c, char *err) {
    addReplyErrorLength(c,err,strlen(err));
}

void addReplyErrorFormat(redisClient *c, const char *fmt, ...) {
    size_t l, j;
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted. */
    l = sdslen(s);
    for (j = 0; j < l; j++) {
        if (s[j] == '\r' || s[j] == '\n') s[j] = ' ';
    }
    addReplyErrorLength(c,s,sdslen(s));
    sdsfree(s);
}


//---------------------------------------------------------------------
// 添加正常响应状态消息到发送缓冲区
//
// +OK\r\n
//---------------------------------------------------------------------
void addReplyStatusLength(redisClient *c, char *s, size_t len) {
    addReplyString(c,"+",1);
    addReplyString(c,s,len);
    addReplyString(c,"\r\n",2);
}

void addReplyStatus(redisClient *c, char *status) {
    addReplyStatusLength(c,status,strlen(status));
}

void addReplyStatusFormat(redisClient *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    addReplyStatusLength(c,s,sdslen(s));
    sdsfree(s);
}

/* Adds an empty object to the reply list that will contain the multi bulk
 * length, which is not known when this function is called. */

//---------------------------------------------------------------------
// 对于响应返回数组长度预先不确定的情况
// 预留一个节点位置，等待确定了数组长度再填充
//---------------------------------------------------------------------
void *addDeferredMultiBulkLength(redisClient *c) {
    /* Note that we install the write event here even if the object is not
     * ready to be sent, since we are sure that before returning to the
     * event loop setDeferredMultiBulkLength() will be called. */
    if (prepareClientToWrite(c) != REDIS_OK) return NULL;
    listAddNodeTail(c->reply,createObject(REDIS_STRING,NULL));
    return listLast(c->reply);
}

/* Populate the length object and try gluing it to the next chunk. */

//---------------------------------------------------------------------
// 填充之前预留的数组长度的值
//
// *<len>\r\n
//---------------------------------------------------------------------
void setDeferredMultiBulkLength(redisClient *c, void *node, long length) {
    listNode *ln = (listNode*)node;
    robj *len, *next;

    /* Abort when *node is NULL (see addDeferredMultiBulkLength). */
    if (node == NULL) return;

    len = listNodeValue(ln);
    len->ptr = sdscatprintf(sdsempty(),"*%ld\r\n",length);
    c->reply_bytes += zmalloc_size_sds(len->ptr);
    if (ln->next != NULL) {
        next = listNodeValue(ln->next);

        /* Only glue when the next node is non-NULL (an sds in this case) */
        // 尝试和下一个节点合并数据
        if (next->ptr != NULL) {
            c->reply_bytes -= zmalloc_size_sds(len->ptr);
            c->reply_bytes -= zmalloc_size_sds(next->ptr);
            len->ptr = sdscatlen(len->ptr,next->ptr,sdslen(next->ptr));
            c->reply_bytes += zmalloc_size_sds(len->ptr);
            listDelNode(c->reply,ln->next);
        }
    }
    asyncCloseClientOnOutputBufferLimitReached(c);
}

/* Add a double as a bulk reply */

//---------------------------------------------------------------------
// $<len>\r\n<double>\r\n
//---------------------------------------------------------------------
void addReplyDouble(redisClient *c, double d) {
    char dbuf[128], sbuf[128];
    int dlen, slen;
    if (isinf(d)) {
        /* Libc in odd systems (Hi Solaris!) will format infinite in a
         * different way, so better to handle it in an explicit way. */
        addReplyBulkCString(c, d > 0 ? "inf" : "-inf");
    } else {
        dlen = snprintf(dbuf,sizeof(dbuf),"%.17g",d);
        slen = snprintf(sbuf,sizeof(sbuf),"$%d\r\n%s\r\n",dlen,dbuf);
        addReplyString(c,sbuf,slen);
    }
}

/* Add a long long as integer reply or bulk len / multi bulk count.
 * Basically this is used to output <prefix><long long><crlf>. */

//---------------------------------------------------------------------
// <prefix><long long><crlf>
//---------------------------------------------------------------------
void addReplyLongLongWithPrefix(redisClient *c, long long ll, char prefix) {
    char buf[128];
    int len;

    /* Things like $3\r\n or *2\r\n are emitted very often by the protocol
     * so we have a few shared objects to use if the integer is small
     * like it is most of the times. */
    if (prefix == '*' && ll < REDIS_SHARED_BULKHDR_LEN) {
        addReply(c,shared.mbulkhdr[ll]);
        return;
    } else if (prefix == '$' && ll < REDIS_SHARED_BULKHDR_LEN) {
        addReply(c,shared.bulkhdr[ll]);
        return;
    }

    buf[0] = prefix;
    len = ll2string(buf+1,sizeof(buf)-1,ll);
    buf[len+1] = '\r';
    buf[len+2] = '\n';
    addReplyString(c,buf,len+3);
}


//---------------------------------------------------------------------
// 发送一个带前缀的整数响应
//---------------------------------------------------------------------
void addReplyLongLong(redisClient *c, long long ll) {
    if (ll == 0)
        addReply(c,shared.czero);
    else if (ll == 1)
        addReply(c,shared.cone);
    else
        addReplyLongLongWithPrefix(c,ll,':');
}

// *<len>\r\n
void addReplyMultiBulkLen(redisClient *c, long length) {
    if (length < REDIS_SHARED_BULKHDR_LEN)
        addReply(c,shared.mbulkhdr[length]);
    else
        addReplyLongLongWithPrefix(c,length,'*');
}

/* Create the length prefix of a bulk reply, example: $2234 */
void addReplyBulkLen(redisClient *c, robj *obj) {
    size_t len;

    if (obj->encoding == REDIS_ENCODING_RAW) {
        len = sdslen(obj->ptr);
    } else {
        long n = (long)obj->ptr;

        /* Compute how many bytes will take this integer as a radix 10 string */
        // 计算整数用字符串表示所需长度
        len = 1;
        if (n < 0) {
            len++;
            n = -n;
        }
        while((n = n/10) != 0) {
            len++;
        }
    }

    if (len < REDIS_SHARED_BULKHDR_LEN)
        // len < 32
        // $<len>\r\n
        addReply(c,shared.bulkhdr[len]);
    else
        addReplyLongLongWithPrefix(c,len,'$');
}

/* Add a Redis Object as a bulk reply */
void addReplyBulk(redisClient *c, robj *obj) {
    addReplyBulkLen(c,obj);
    addReply(c,obj);
    addReply(c,shared.crlf);
}

/* Add a C buffer as bulk reply */

//---------------------------------------------------------------------
// $<len>\r\n<body>\r\n
//---------------------------------------------------------------------
void addReplyBulkCBuffer(redisClient *c, void *p, size_t len) {
    addReplyLongLongWithPrefix(c,len,'$');
    addReplyString(c,p,len);
    addReply(c,shared.crlf);
}

/* Add a C nul term string as bulk reply */

//---------------------------------------------------------------------
// 添加块字符串响应
//
// $<len>\r\n<body>\r\n
//---------------------------------------------------------------------
void addReplyBulkCString(redisClient *c, char *s) {
    if (s == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        addReplyBulkCBuffer(c,s,strlen(s));
    }
}

/* Add a long long as a bulk reply */
// $<len>\r\n<long long>\r\n
void addReplyBulkLongLong(redisClient *c, long long ll) {
    char buf[64];
    int len;

    len = ll2string(buf,64,ll);
    addReplyBulkCBuffer(c,buf,len);
}

/* Copy 'src' client output buffers into 'dst' client output buffers.
 * The function takes care of freeing the old output buffers of the
 * destination client. */

//---------------------------------------------------------------------
// 将 src 客户端的发送缓存复制到 dst 客户端的发送缓存中
//---------------------------------------------------------------------
void copyClientOutputBuffer(redisClient *dst, redisClient *src) {
    // 释放 dst 原来的 dst->reply
    listRelease(dst->reply);
    // 从 src 复制一份响应缓冲区链表
    dst->reply = listDup(src->reply);

    // 复制静态缓冲区
    memcpy(dst->buf,src->buf,src->bufpos);
    // 偏移量也复制过来
    dst->bufpos = src->bufpos;

    // 响应缓冲区链表中数据的总字节数也复制过来
    dst->reply_bytes = src->reply_bytes;
}

#define MAX_ACCEPTS_PER_CALL 1000

//---------------------------------------------------------------------
// 新来客户端连接处理函数
//---------------------------------------------------------------------
static void acceptCommonHandler(int fd, int flags) {
    redisClient *c;
    // 创建一个 redisClient
    if ((c = createClient(fd)) == NULL) {
        redisLog(REDIS_WARNING,
            "Error registering fd event for the new client: %s (fd=%d)",
            strerror(errno),fd);
        close(fd); /* May be already closed, just ignore errors */
        return;
    }
    /* If maxclient directive is set and this is one client more... close the
     * connection. Note that we create the client instead to check before
     * for this condition, since now the socket is already set in non-blocking
     * mode and we can send an error for free using the Kernel I/O */
    // 判断是否超过了最大连接数
    if (listLength(server.clients) > server.maxclients) {
        char *err = "-ERR max number of clients reached\r\n";

        /* That's a best effort error message, don't check write errors */
        // 尝试发送一条错误消息给客户端
        if (write(c->fd,err,strlen(err)) == -1) {
            /* Nothing to do, Just to avoid the warning... */
        }
        server.stat_rejected_conn++;
        // 销毁 redisClient
        freeClient(c);
        return;
    }
    // 更新统计信息连接次数
    server.stat_numconnections++;
    // 设置连接方式标志位
    c->flags |= flags;
}


//---------------------------------------------------------------------
// 接受客户端连接事件回调
//---------------------------------------------------------------------
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    // MAX_ACCEPTS_PER_CALL 每次调用尝试接受客户端最大个数
    int cport, cfd, max = MAX_ACCEPTS_PER_CALL;
    char cip[REDIS_IP_STR_LEN];
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    REDIS_NOTUSED(privdata);

    while(max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                redisLog(REDIS_WARNING,
                    "Accepting client connection: %s", server.neterr);
            return;
        }
        redisLog(REDIS_VERBOSE,"Accepted %s:%d", cip, cport);
        // 调用新来客户端连接处理函数
        acceptCommonHandler(cfd,0);
    }
}

void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cfd, max = MAX_ACCEPTS_PER_CALL;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    REDIS_NOTUSED(privdata);

    while(max--) {
        cfd = anetUnixAccept(server.neterr, fd);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                redisLog(REDIS_WARNING,
                    "Accepting client connection: %s", server.neterr);
            return;
        }
        redisLog(REDIS_VERBOSE,"Accepted connection to %s", server.unixsocket);
        acceptCommonHandler(cfd,REDIS_UNIX_SOCKET);
    }
}

static void freeClientArgv(redisClient *c) {
    int j;
    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    c->argc = 0;
    c->cmd = NULL;
}

/* Close all the slaves connections. This is useful in chained replication
 * when we resync with our own master and want to force all our slaves to
 * resync with us as well. */
void disconnectSlaves(void) {
    while (listLength(server.slaves)) {
        listNode *ln = listFirst(server.slaves);
        freeClient((redisClient*)ln->value);
    }
}


//---------------------------------------------------------------------
// 释放客户端
//
// 套接字也会随之关闭
//---------------------------------------------------------------------
void freeClient(redisClient *c) {
    listNode *ln;

    /* If this is marked as current client unset it */
    if (server.current_client == c) server.current_client = NULL;

    /* If it is our master that's beging disconnected we should make sure
     * to cache the state to try a partial resynchronization later.
     *
     * Note that before doing this we make sure that the client is not in
     * some unexpected state, by checking its flags. */
    if (server.master && c->flags & REDIS_MASTER) {
        redisLog(REDIS_WARNING,"Connection with master lost.");
        if (!(c->flags & (REDIS_CLOSE_AFTER_REPLY|
                          REDIS_CLOSE_ASAP|
                          REDIS_BLOCKED|
                          REDIS_UNBLOCKED)))
        {
            replicationCacheMaster(c);
            return;
        }
    }

    /* Log link disconnection with slave */
    if ((c->flags & REDIS_SLAVE) && !(c->flags & REDIS_MONITOR)) {
        redisLog(REDIS_WARNING,"Connection with slave %s lost.",
            replicationGetSlaveName(c));
    }

    /* Free the query buffer */
    sdsfree(c->querybuf);
    c->querybuf = NULL;

    /* Deallocate structures used to block on blocking ops. */
    if (c->flags & REDIS_BLOCKED)
        unblockClientWaitingData(c);
    dictRelease(c->bpop.keys);

    /* UNWATCH all the keys */
    unwatchAllKeys(c);
    listRelease(c->watched_keys);

    /* Unsubscribe from all the pubsub channels */
    pubsubUnsubscribeAllChannels(c,0);
    pubsubUnsubscribeAllPatterns(c,0);
    dictRelease(c->pubsub_channels);
    listRelease(c->pubsub_patterns);

    /* Close socket, unregister events, and remove list of replies and
     * accumulated arguments. */
    // 移除时间监听，关闭套接字
    if (c->fd != -1) {
        aeDeleteFileEvent(server.el,c->fd,AE_READABLE);
        aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);
        close(c->fd);
    }

    // 释放响应缓冲区链表
    listRelease(c->reply);

    // 释放命令参数数组
    freeClientArgv(c);

    /* Remove from the list of clients */
    // 从服务器的客户端链表中移除自己
    if (c->fd != -1) {
        ln = listSearchKey(server.clients,c);
        redisAssert(ln != NULL);
        listDelNode(server.clients,ln);
    }

    /* When client was just unblocked because of a blocking operation,
     * remove it from the list of unblocked clients. */
    if (c->flags & REDIS_UNBLOCKED) {
        ln = listSearchKey(server.unblocked_clients,c);
        redisAssert(ln != NULL);
        listDelNode(server.unblocked_clients,ln);
    }

    /* Master/slave cleanup Case 1:
     * we lost the connection with a slave. */
    if (c->flags & REDIS_SLAVE) {
        if (c->replstate == REDIS_REPL_SEND_BULK) {
            if (c->repldbfd != -1) close(c->repldbfd);
            if (c->replpreamble) sdsfree(c->replpreamble);
        }
        list *l = (c->flags & REDIS_MONITOR) ? server.monitors : server.slaves;
        ln = listSearchKey(l,c);
        redisAssert(ln != NULL);
        listDelNode(l,ln);
        /* We need to remember the time when we started to have zero
         * attached slaves, as after some time we'll free the replication
         * backlog. */
        if (c->flags & REDIS_SLAVE && listLength(server.slaves) == 0)
            server.repl_no_slaves_since = server.unixtime;
        refreshGoodSlavesCount();
    }

    /* Master/slave cleanup Case 2:
     * we lost the connection with the master. */
    if (c->flags & REDIS_MASTER) replicationHandleMasterDisconnection();

    /* If this client was scheduled for async freeing we need to remove it
     * from the queue. */
    // 从异步延迟销毁客户端链表中删除自己
    if (c->flags & REDIS_CLOSE_ASAP) {
        ln = listSearchKey(server.clients_to_close,c);
        redisAssert(ln != NULL);
        listDelNode(server.clients_to_close,ln);
    }

    /* Release other dynamically allocated client structure fields,
     * and finally release the client structure itself. */
    if (c->name) decrRefCount(c->name);
    zfree(c->argv);
    // 清除事务状态信息
    freeClientMultiState(c);
    sdsfree(c->peerid);

    // 释放 redisClient 结构本身占用的内存
    zfree(c);
}

/* Schedule a client to free it at a safe time in the serverCron() function.
 * This function is useful when we need to terminate a client but we are in
 * a context where calling freeClient() is not possible, because the client
 * should be valid for the continuation of the flow of the program. */

//---------------------------------------------------------------------
// 异步销毁客户端连接（加到延迟销毁队列中）
//---------------------------------------------------------------------
void freeClientAsync(redisClient *c) {
    // 已设置标志或者为 lua 客户端时直接返回
    if (c->flags & REDIS_CLOSE_ASAP || c->flags & REDIS_LUA_CLIENT) return;
    // 设置 REDIS_CLOSE_ASAP 标志
    c->flags |= REDIS_CLOSE_ASAP;
    // 追加到 server.clients_to_close 待销毁客户端连接链表的尾部
    listAddNodeTail(server.clients_to_close,c);
}


//---------------------------------------------------------------------
// 遍历 server.clients_to_close 并关闭需要延迟销毁的客户端
//---------------------------------------------------------------------
void freeClientsInAsyncFreeQueue(void) {
    while (listLength(server.clients_to_close)) {
        listNode *ln = listFirst(server.clients_to_close);
        redisClient *c = listNodeValue(ln);

        // 清除 REDIS_CLOSE_ASAP 标志
        c->flags &= ~REDIS_CLOSE_ASAP;
        // 释放客户端
        freeClient(c);
        // 从链表中移除
        listDelNode(server.clients_to_close,ln);
    }
}


//---------------------------------------------------------------------
// 套接字可写事件回调
//---------------------------------------------------------------------
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *c = privdata;
    int nwritten = 0, totwritten = 0, objlen;
    size_t objmem;
    robj *o;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    // 循环发送数据
    while(c->bufpos > 0 || listLength(c->reply)) {
        // 先发送静态缓冲区的数据
        if (c->bufpos > 0) {
            // sentlen 为静态发送缓冲区已发送数据长度
            nwritten = write(fd,c->buf+c->sentlen,c->bufpos-c->sentlen);
            if (nwritten <= 0) break;
            c->sentlen += nwritten;
            totwritten += nwritten;

            /* If the buffer was sent, set bufpos to zero to continue with
             * the remainder of the reply. */
            // 发完了，重置 c->bufpos 和 c->sentlen
            if (c->sentlen == c->bufpos) {
                c->bufpos = 0;
                c->sentlen = 0;
            }
        // 再发送缓冲区链表中的数据
        } else {
            // 从链表头部开始每发送完一个节点就删除一个
            // 继续从头部开始
            o = listNodeValue(listFirst(c->reply));
            objlen = sdslen(o->ptr);
            objmem = zmalloc_size_sds(o->ptr);

            // 该节点数据是空的，删除节点并跳过
            if (objlen == 0) {
                listDelNode(c->reply,listFirst(c->reply));
                continue;
            }

            nwritten = write(fd, ((char*)o->ptr)+c->sentlen,objlen-c->sentlen);
            // 无法继续发送或者发送失败就跳出
            if (nwritten <= 0) break;

            // c->sentlen 为当前节点已发送字节数
            c->sentlen += nwritten;
            totwritten += nwritten;

            /* If we fully sent the object on head go to the next one */
            // 当前节点数据已发完
            if (c->sentlen == objlen) {
                // 删除节点
                listDelNode(c->reply,listFirst(c->reply));
                // 重置 c->sentlen
                c->sentlen = 0;
                c->reply_bytes -= objmem;
            }
        }

        /* Note that we avoid to send more than REDIS_MAX_WRITE_PER_EVENT
         * bytes, in a single threaded server it's a good idea to serve
         * other clients as well, even if a very large request comes from
         * super fast link that is always able to accept data (in real world
         * scenario think about 'KEYS *' against the loopback interface).
         *
         * However if we are over the maxmemory limit we ignore that and
         * just deliver as much data as it is possible to deliver. */
        server.stat_net_output_bytes += totwritten;
        // 检测单次事件数据发送上限 REDIS_MAX_WRITE_PER_EVENT
        // 避免单次事件发送数据太大占用时间较多，其他连接上的请求无法及时响应
        //
        // 例外情况：
        //   使用内存超过上限（尽快发送完数据给客户端，好回收内存）
        if (totwritten > REDIS_MAX_WRITE_PER_EVENT &&
            (server.maxmemory == 0 ||
             zmalloc_used_memory() < server.maxmemory)) break;
    }

    if (nwritten == -1) {
        if (errno == EAGAIN) {
            // 套接字当前不可写，等待下一次再写
            nwritten = 0;
        } else {
            // 其他错误处理
            redisLog(REDIS_VERBOSE,
                "Error writing to client: %s", strerror(errno));
            freeClient(c);
            return;
        }
    }

    // totwritten 已发送字节数
    if (totwritten > 0) {
        /* For clients representing masters we don't count sending data
         * as an interaction, since we always send REPLCONF ACK commands
         * that take some time to just fill the socket output buffer.
         * We just rely on data / pings received for timeout detection. */
        // 更新最近数据交互时间
        if (!(c->flags & REDIS_MASTER)) c->lastinteraction = server.unixtime;
    }

    // 缓冲区数据已发送完，先移除可写事件监听，避免 busy loop
    if (c->bufpos == 0 && listLength(c->reply) == 0) {
        // 重置 c->sentlen
        c->sentlen = 0;

        // 移除可写事件监听（等到下次发送缓冲区有数据时会添加可写事件监听）
        aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);

        /* Close connection after entire reply has been sent. */
        // 检测标志 REDIS_CLOSE_AFTER_REPLY
        // 为真时发送完响应就直接销毁客户端
        if (c->flags & REDIS_CLOSE_AFTER_REPLY) freeClient(c);
    }
}

/* resetClient prepare the client to process the next command */

//---------------------------------------------------------------------
// 重置客户端的一些属性，好接着解析处理下一条命令
//---------------------------------------------------------------------
void resetClient(redisClient *c) {
    freeClientArgv(c);
    c->reqtype = 0;
    c->multibulklen = 0;
    c->bulklen = -1;
    /* We clear the ASKING flag as well if we are not inside a MULTI. */
    if (!(c->flags & REDIS_MULTI)) c->flags &= (~REDIS_ASKING);
}


//---------------------------------------------------------------------
// 解析内联命令（即 telnet 那种单行用空格分开的形式）
//
// 填充 argc,argv
//---------------------------------------------------------------------
int processInlineBuffer(redisClient *c) {
    char *newline;
    int argc, j;
    sds *argv, aux;
    size_t querylen;

    /* Search for end of line */
    newline = strchr(c->querybuf,'\n');

    /* Nothing to do without a \r\n */
    if (newline == NULL) {
        if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
            addReplyError(c,"Protocol error: too big inline request");
            setProtocolError(c,0);
        }
        return REDIS_ERR;
    }

    /* Handle the \r\n case. */
    if (newline && newline != c->querybuf && *(newline-1) == '\r')
        newline--;

    /* Split the input buffer up to the \r\n */
    // 分离出命令部分
    querylen = newline-(c->querybuf);
    aux = sdsnewlen(c->querybuf,querylen);
    // 用 sdssplitargs 分割参数
    argv = sdssplitargs(aux,&argc);
    sdsfree(aux);
    if (argv == NULL) {
        addReplyError(c,"Protocol error: unbalanced quotes in request");
        setProtocolError(c,0);
        return REDIS_ERR;
    }

    /* Newline from slaves can be used to refresh the last ACK time.
     * This is useful for a slave to ping back while loading a big
     * RDB file. */
    if (querylen == 0 && c->flags & REDIS_SLAVE)
        c->repl_ack_time = server.unixtime;

    /* Leave data after the first line of the query in the buffer */
    sdsrange(c->querybuf,querylen+2,-1);

    /* Setup argv array on client structure */
    if (argc) {
        // 释放掉旧的，分配新的给 argv
        if (c->argv) zfree(c->argv);
        c->argv = zmalloc(sizeof(robj*)*argc);
    }

    /* Create redis objects for all arguments. */
    for (c->argc = 0, j = 0; j < argc; j++) {
        if (sdslen(argv[j])) {
            // 创建 redisObject 存到 c->argv 中
            c->argv[c->argc] = createObject(REDIS_STRING,argv[j]);
            c->argc++;
        } else {
            // 跳过为空的参数
            sdsfree(argv[j]);
        }
    }
    zfree(argv);
    return REDIS_OK;
}

/* Helper function. Trims query buffer to make the function that processes
 * multi bulk requests idempotent. */
static void setProtocolError(redisClient *c, int pos) {
    if (server.verbosity <= REDIS_VERBOSE) {
        sds client = catClientInfoString(sdsempty(),c);
        redisLog(REDIS_VERBOSE,
            "Protocol error from client: %s", client);
        sdsfree(client);
    }
    c->flags |= REDIS_CLOSE_AFTER_REPLY;
    sdsrange(c->querybuf,pos,-1);
}


//---------------------------------------------------------------------
// 解析 RESP 协议的命令
//
// *3\r\n$3\r\nSET\r\n$3\r\nMSG\r\n$5\r\nHELLO\r\n
//
// argv[0] = SET
// argv[1] = MSG
// argv[2] = HELLO
//---------------------------------------------------------------------
int processMultibulkBuffer(redisClient *c) {
    char *newline = NULL;
    int pos = 0, ok;
    long long ll;

    if (c->multibulklen == 0) {
        /* The client should have been reset */
        redisAssertWithInfo(c,NULL,c->argc == 0);

        /* Multi bulk length cannot be read without a \r\n */
        newline = strchr(c->querybuf,'\r');
        if (newline == NULL) {
            if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
                addReplyError(c,"Protocol error: too big mbulk count string");
                setProtocolError(c,0);
            }
            return REDIS_ERR;
        }

        /* Buffer should also contain \n */
        if (newline-(c->querybuf) > ((signed)sdslen(c->querybuf)-2))
            return REDIS_ERR;

        /* We know for sure there is a whole line since newline != NULL,
         * so go ahead and find out the multi bulk length. */
        redisAssertWithInfo(c,NULL,c->querybuf[0] == '*');
        ok = string2ll(c->querybuf+1,newline-(c->querybuf+1),&ll);
        if (!ok || ll > 1024*1024) {
            addReplyError(c,"Protocol error: invalid multibulk length");
            setProtocolError(c,pos);
            return REDIS_ERR;
        }

        pos = (newline-c->querybuf)+2;
        if (ll <= 0) {
            sdsrange(c->querybuf,pos,-1);
            return REDIS_OK;
        }

        c->multibulklen = ll;

        /* Setup argv array on client structure */
        if (c->argv) zfree(c->argv);
        c->argv = zmalloc(sizeof(robj*)*c->multibulklen);
    }

    redisAssertWithInfo(c,NULL,c->multibulklen > 0);
    while(c->multibulklen) {
        /* Read bulk length if unknown */
        if (c->bulklen == -1) {
            newline = strchr(c->querybuf+pos,'\r');
            if (newline == NULL) {
                if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
                    addReplyError(c,
                        "Protocol error: too big bulk count string");
                    setProtocolError(c,0);
                    return REDIS_ERR;
                }
                break;
            }

            /* Buffer should also contain \n */
            if (newline-(c->querybuf) > ((signed)sdslen(c->querybuf)-2))
                break;

            if (c->querybuf[pos] != '$') {
                addReplyErrorFormat(c,
                    "Protocol error: expected '$', got '%c'",
                    c->querybuf[pos]);
                setProtocolError(c,pos);
                return REDIS_ERR;
            }

            ok = string2ll(c->querybuf+pos+1,newline-(c->querybuf+pos+1),&ll);
            if (!ok || ll < 0 || ll > 512*1024*1024) {
                addReplyError(c,"Protocol error: invalid bulk length");
                setProtocolError(c,pos);
                return REDIS_ERR;
            }

            pos += newline-(c->querybuf+pos)+2;
            if (ll >= REDIS_MBULK_BIG_ARG) {
                size_t qblen;

                /* If we are going to read a large object from network
                 * try to make it likely that it will start at c->querybuf
                 * boundary so that we can optimize object creation
                 * avoiding a large copy of data. */
                sdsrange(c->querybuf,pos,-1);
                pos = 0;
                qblen = sdslen(c->querybuf);
                /* Hint the sds library about the amount of bytes this string is
                 * going to contain. */
                if (qblen < (size_t)ll+2)
                    c->querybuf = sdsMakeRoomFor(c->querybuf,ll+2-qblen);
            }
            c->bulklen = ll;
        }

        /* Read bulk argument */
        if (sdslen(c->querybuf)-pos < (unsigned)(c->bulklen+2)) {
            /* Not enough data (+2 == trailing \r\n) */
            break;
        } else {
            /* Optimization: if the buffer contains JUST our bulk element
             * instead of creating a new object by *copying* the sds we
             * just use the current sds string. */
            if (pos == 0 &&
                c->bulklen >= REDIS_MBULK_BIG_ARG &&
                (signed) sdslen(c->querybuf) == c->bulklen+2)
            {
                c->argv[c->argc++] = createObject(REDIS_STRING,c->querybuf);
                sdsIncrLen(c->querybuf,-2); /* remove CRLF */
                c->querybuf = sdsempty();
                /* Assume that if we saw a fat argument we'll see another one
                 * likely... */
                c->querybuf = sdsMakeRoomFor(c->querybuf,c->bulklen+2);
                pos = 0;
            } else {
                c->argv[c->argc++] =
                    createStringObject(c->querybuf+pos,c->bulklen);
                pos += c->bulklen+2;
            }
            c->bulklen = -1;
            c->multibulklen--;
        }
    }

    /* Trim to pos */
    if (pos) sdsrange(c->querybuf,pos,-1);

    /* We're done when c->multibulk == 0 */
    if (c->multibulklen == 0) return REDIS_OK;

    /* Still not read to process the command */
    return REDIS_ERR;
}


//---------------------------------------------------------------------
// 从请求缓冲区中解析出命令
//---------------------------------------------------------------------
void processInputBuffer(redisClient *c) {
    /* Keep processing while there is something in the input buffer */
    // 循环处理
    while(sdslen(c->querybuf)) {
        /* Immediately abort if the client is in the middle of something. */
        // 当前客户端出于阻塞状态，不继续处理命令直接返回
        if (c->flags & REDIS_BLOCKED) return;

        /* REDIS_CLOSE_AFTER_REPLY closes the connection once the reply is
         * written to the client. Make sure to not let the reply grow after
         * this flag has been set (i.e. don't process more commands). */
        // 客户端设置了关闭标志，不继续处理命令直接返回
        if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

        /* Determine request type when unknown. */
        // 根据首字节检测请求类型
        if (!c->reqtype) {
            if (c->querybuf[0] == '*') {
                // multi bulk
                c->reqtype = REDIS_REQ_MULTIBULK;
            } else {
                // inline
                c->reqtype = REDIS_REQ_INLINE;
            }
        }

        // 解析命令，失败直接返回，等待读到更多的数据包
        if (c->reqtype == REDIS_REQ_INLINE) {
            if (processInlineBuffer(c) != REDIS_OK) break;
        } else if (c->reqtype == REDIS_REQ_MULTIBULK) {
            if (processMultibulkBuffer(c) != REDIS_OK) break;
        } else {
            // panic?
            redisPanic("Unknown request type");
        }

        /* Multibulk processing could see a <= 0 length. */
        if (c->argc == 0) {
            // 空命令，重置客户端，接着等待处理下一条命令
            resetClient(c);
        } else {
            /* Only reset the client when the command was executed. */
            // 读到一条命令后，处理命令
            if (processCommand(c) == REDIS_OK)
                // 处理成功则重置客户端，接着解析处理下一条命令
                resetClient(c);
        }
    }
}


//---------------------------------------------------------------------
// 处理请求数据包读取
//---------------------------------------------------------------------
void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *c = (redisClient*) privdata;
    int nread, readlen;
    size_t qblen;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    server.current_client = c;
    // 一次尝试读 16k
    readlen = REDIS_IOBUF_LEN;
    /* If this is a multi bulk request, and we are processing a bulk reply
     * that is large enough, try to maximize the probability that the query
     * buffer contains exactly the SDS string representing the object, even
     * at the risk of requiring more read(2) calls. This way the function
     * processMultiBulkBuffer() can avoid copying buffers to create the
     * Redis Object representing the argument. */
    if (c->reqtype == REDIS_REQ_MULTIBULK && c->multibulklen && c->bulklen != -1
        && c->bulklen >= REDIS_MBULK_BIG_ARG)
    {
        int remaining = (unsigned)(c->bulklen+2)-sdslen(c->querybuf);

        if (remaining < readlen) readlen = remaining;
    }

    // qblen 当前请求缓冲区已读数据包大小
    qblen = sdslen(c->querybuf);
    // 计算可 peek 的缓冲区长度
    if (c->querybuf_peak < qblen) c->querybuf_peak = qblen;
    // 为下面的读操作申请更多的缓冲区
    c->querybuf = sdsMakeRoomFor(c->querybuf, readlen);
    // socket read
    // 每次可读事件到来，只读一次
    nread = read(fd, c->querybuf+qblen, readlen);
    if (nread == -1) {
        // 读出错了
        if (errno == EAGAIN) {
            // EAGAIN 没有数据可读，等待下一次可读事件触发
            nread = 0;
        } else {
            // 其他错误处理
            redisLog(REDIS_VERBOSE, "Reading from client: %s",strerror(errno));
            freeClient(c);
            return;
        }
    } else if (nread == 0) {
        // 对端断开了连接
        redisLog(REDIS_VERBOSE, "Client closed connection");
        // 销毁客户端连接对象并返回
        freeClient(c);
        return;
    }

    if (nread) {
        // 读到了数据
        // 修正 querybuf 长度
        sdsIncrLen(c->querybuf,nread);
        // 更新数据交互时间记录
        c->lastinteraction = server.unixtime;
        if (c->flags & REDIS_MASTER) c->reploff += nread;
        server.stat_net_input_bytes += nread;
    } else {
        // 没读到数据就返回
        server.current_client = NULL;
        return;
    }

    // 缓冲区大小超过了设定的最大值
    if (sdslen(c->querybuf) > server.client_max_querybuf_len) {
        sds ci = catClientInfoString(sdsempty(),c), bytes = sdsempty();

        bytes = sdscatrepr(bytes,c->querybuf,64);
        // 记录日志并销毁客户端连接对象
        redisLog(REDIS_WARNING,"Closing client that reached max query buffer length: %s (qbuf initial bytes: %s)", ci, bytes);
        sdsfree(ci);
        sdsfree(bytes);
        freeClient(c);
        return;
    }

    // 尝试解析当前已读到的数据包
    // 本次读到的数据包可能含有多条命令，会全部一次性处理完
    processInputBuffer(c);
    server.current_client = NULL;
}

void getClientsMaxBuffers(unsigned long *longest_output_list,
                          unsigned long *biggest_input_buffer) {
    redisClient *c;
    listNode *ln;
    listIter li;
    unsigned long lol = 0, bib = 0;

    listRewind(server.clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        c = listNodeValue(ln);

        if (listLength(c->reply) > lol) lol = listLength(c->reply);
        if (sdslen(c->querybuf) > bib) bib = sdslen(c->querybuf);
    }
    *longest_output_list = lol;
    *biggest_input_buffer = bib;
}

/* This is a helper function for genClientPeerId().
 * It writes the specified ip/port to "peerid" as a null termiated string
 * in the form ip:port if ip does not contain ":" itself, otherwise
 * [ip]:port format is used (for IPv6 addresses basically). */
void formatPeerId(char *peerid, size_t peerid_len, char *ip, int port) {
    if (strchr(ip,':'))
        snprintf(peerid,peerid_len,"[%s]:%d",ip,port);
    else
        snprintf(peerid,peerid_len,"%s:%d",ip,port);
}

/* A Redis "Peer ID" is a colon separated ip:port pair.
 * For IPv4 it's in the form x.y.z.k:port, example: "127.0.0.1:1234".
 * For IPv6 addresses we use [] around the IP part, like in "[::1]:1234".
 * For Unix sockets we use path:0, like in "/tmp/redis:0".
 *
 * A Peer ID always fits inside a buffer of REDIS_PEER_ID_LEN bytes, including
 * the null term.
 *
 * The function returns REDIS_OK on succcess, and REDIS_ERR on failure.
 *
 * On failure the function still populates 'peerid' with the "?:0" string
 * in case you want to relax error checking or need to display something
 * anyway (see anetPeerToString implementation for more info). */
int genClientPeerId(redisClient *client, char *peerid, size_t peerid_len) {
    char ip[REDIS_IP_STR_LEN];
    int port;

    if (client->flags & REDIS_UNIX_SOCKET) {
        /* Unix socket client. */
        snprintf(peerid,peerid_len,"%s:0",server.unixsocket);
        return REDIS_OK;
    } else {
        /* TCP client. */
        int retval = anetPeerToString(client->fd,ip,sizeof(ip),&port);
        formatPeerId(peerid,peerid_len,ip,port);
        return (retval == -1) ? REDIS_ERR : REDIS_OK;
    }
}

/* This function returns the client peer id, by creating and caching it
 * if client->peerid is NULL, otherwise returning the cached value.
 * The Peer ID never changes during the life of the client, however it
 * is expensive to compute. */
char *getClientPeerId(redisClient *c) {
    char peerid[REDIS_PEER_ID_LEN];

    if (c->peerid == NULL) {
        genClientPeerId(c,peerid,sizeof(peerid));
        c->peerid = sdsnew(peerid);
    }
    return c->peerid;
}

/* Concatenate a string representing the state of a client in an human
 * readable format, into the sds string 's'. */
sds catClientInfoString(sds s, redisClient *client) {
    char flags[16], events[3], *p;
    int emask;

    p = flags;
    if (client->flags & REDIS_SLAVE) {
        if (client->flags & REDIS_MONITOR)
            *p++ = 'O';
        else
            *p++ = 'S';
    }
    if (client->flags & REDIS_MASTER) *p++ = 'M';
    if (client->flags & REDIS_MULTI) *p++ = 'x';
    if (client->flags & REDIS_BLOCKED) *p++ = 'b';
    if (client->flags & REDIS_DIRTY_CAS) *p++ = 'd';
    if (client->flags & REDIS_CLOSE_AFTER_REPLY) *p++ = 'c';
    if (client->flags & REDIS_UNBLOCKED) *p++ = 'u';
    if (client->flags & REDIS_CLOSE_ASAP) *p++ = 'A';
    if (client->flags & REDIS_UNIX_SOCKET) *p++ = 'U';
    if (p == flags) *p++ = 'N';
    *p++ = '\0';

    emask = client->fd == -1 ? 0 : aeGetFileEvents(server.el,client->fd);
    p = events;
    if (emask & AE_READABLE) *p++ = 'r';
    if (emask & AE_WRITABLE) *p++ = 'w';
    *p = '\0';
    return sdscatfmt(s,
        "id=%U addr=%s fd=%i name=%s age=%I idle=%I flags=%s db=%i sub=%i psub=%i multi=%i qbuf=%U qbuf-free=%U obl=%U oll=%U omem=%U events=%s cmd=%s",
        (unsigned long long) client->id,
        getClientPeerId(client),
        client->fd,
        client->name ? (char*)client->name->ptr : "",
        (long long)(server.unixtime - client->ctime),
        (long long)(server.unixtime - client->lastinteraction),
        flags,
        client->db->id,
        (int) dictSize(client->pubsub_channels),
        (int) listLength(client->pubsub_patterns),
        (client->flags & REDIS_MULTI) ? client->mstate.count : -1,
        (unsigned long long) sdslen(client->querybuf),
        (unsigned long long) sdsavail(client->querybuf),
        (unsigned long long) client->bufpos,
        (unsigned long long) listLength(client->reply),
        (unsigned long long) getClientOutputBufferMemoryUsage(client),
        events,
        client->lastcmd ? client->lastcmd->name : "NULL");
}

sds getAllClientsInfoString(void) {
    listNode *ln;
    listIter li;
    redisClient *client;
    sds o = sdsempty();

    o = sdsMakeRoomFor(o,200*listLength(server.clients));
    listRewind(server.clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        client = listNodeValue(ln);
        o = catClientInfoString(o,client);
        o = sdscatlen(o,"\n",1);
    }
    return o;
}

void clientCommand(redisClient *c) {
    listNode *ln;
    listIter li;
    redisClient *client;

    if (!strcasecmp(c->argv[1]->ptr,"list") && c->argc == 2) {
        /* CLIENT LIST */
        sds o = getAllClientsInfoString();
        addReplyBulkCBuffer(c,o,sdslen(o));
        sdsfree(o);
    } else if (!strcasecmp(c->argv[1]->ptr,"kill")) {
        /* CLIENT KILL <ip:port>
         * CLIENT KILL <option> [value] ... <option> [value] */
        char *addr = NULL;
        int type = -1;
        uint64_t id = 0;
        int skipme = 1;
        int killed = 0, close_this_client = 0;

        if (c->argc == 3) {
            /* Old style syntax: CLIENT KILL <addr> */
            addr = c->argv[2]->ptr;
            skipme = 0; /* With the old form, you can kill yourself. */
        } else if (c->argc > 3) {
            int i = 2; /* Next option index. */

            /* New style syntax: parse options. */
            while(i < c->argc) {
                int moreargs = c->argc > i+1;

                if (!strcasecmp(c->argv[i]->ptr,"id") && moreargs) {
                    long long tmp;

                    if (getLongLongFromObjectOrReply(c,c->argv[i+1],&tmp,NULL)
                        != REDIS_OK) return;
                    id = tmp;
                } else if (!strcasecmp(c->argv[i]->ptr,"type") && moreargs) {
                    type = getClientTypeByName(c->argv[i+1]->ptr);
                    if (type == -1) {
                        addReplyErrorFormat(c,"Unknown client type '%s'",
                            (char*) c->argv[i+1]->ptr);
                        return;
                    }
                } else if (!strcasecmp(c->argv[i]->ptr,"addr") && moreargs) {
                    addr = c->argv[i+1]->ptr;
                } else if (!strcasecmp(c->argv[i]->ptr,"skipme") && moreargs) {
                    if (!strcasecmp(c->argv[i+1]->ptr,"yes")) {
                        skipme = 1;
                    } else if (!strcasecmp(c->argv[i+1]->ptr,"no")) {
                        skipme = 0;
                    } else {
                        addReply(c,shared.syntaxerr);
                        return;
                    }
                } else {
                    addReply(c,shared.syntaxerr);
                    return;
                }
                i += 2;
            }
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }

        /* Iterate clients killing all the matching clients. */
        listRewind(server.clients,&li);
        while ((ln = listNext(&li)) != NULL) {
            client = listNodeValue(ln);
            if (addr && strcmp(getClientPeerId(client),addr) != 0) continue;
            if (type != -1 &&
                (client->flags & REDIS_MASTER ||
                 getClientType(client) != type)) continue;
            if (id != 0 && client->id != id) continue;
            if (c == client && skipme) continue;

            /* Kill it. */
            if (c == client) {
                close_this_client = 1;
            } else {
                freeClient(client);
            }
            killed++;
        }

        /* Reply according to old/new format. */
        if (c->argc == 3) {
            if (killed == 0)
                addReplyError(c,"No such client");
            else
                addReply(c,shared.ok);
        } else {
            addReplyLongLong(c,killed);
        }

        /* If this client has to be closed, flag it as CLOSE_AFTER_REPLY
         * only after we queued the reply to its output buffers. */
        if (close_this_client) c->flags |= REDIS_CLOSE_AFTER_REPLY;
    } else if (!strcasecmp(c->argv[1]->ptr,"setname") && c->argc == 3) {
        int j, len = sdslen(c->argv[2]->ptr);
        char *p = c->argv[2]->ptr;

        /* Setting the client name to an empty string actually removes
         * the current name. */
        if (len == 0) {
            if (c->name) decrRefCount(c->name);
            c->name = NULL;
            addReply(c,shared.ok);
            return;
        }

        /* Otherwise check if the charset is ok. We need to do this otherwise
         * CLIENT LIST format will break. You should always be able to
         * split by space to get the different fields. */
        for (j = 0; j < len; j++) {
            if (p[j] < '!' || p[j] > '~') { /* ASCII is assumed. */
                addReplyError(c,
                    "Client names cannot contain spaces, "
                    "newlines or special characters.");
                return;
            }
        }
        if (c->name) decrRefCount(c->name);
        c->name = c->argv[2];
        incrRefCount(c->name);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"getname") && c->argc == 2) {
        if (c->name)
            addReplyBulk(c,c->name);
        else
            addReply(c,shared.nullbulk);
    } else {
        addReplyError(c, "Syntax error, try CLIENT (LIST | KILL ip:port | GETNAME | SETNAME connection-name)");
    }
}

/* Rewrite the command vector of the client. All the new objects ref count
 * is incremented. The old command vector is freed, and the old objects
 * ref count is decremented. */
void rewriteClientCommandVector(redisClient *c, int argc, ...) {
    va_list ap;
    int j;
    robj **argv; /* The new argument vector */

    argv = zmalloc(sizeof(robj*)*argc);
    va_start(ap,argc);
    for (j = 0; j < argc; j++) {
        robj *a;

        a = va_arg(ap, robj*);
        argv[j] = a;
        incrRefCount(a);
    }
    /* We free the objects in the original vector at the end, so we are
     * sure that if the same objects are reused in the new vector the
     * refcount gets incremented before it gets decremented. */
    for (j = 0; j < c->argc; j++) decrRefCount(c->argv[j]);
    zfree(c->argv);
    /* Replace argv and argc with our new versions. */
    c->argv = argv;
    c->argc = argc;
    c->cmd = lookupCommandOrOriginal(c->argv[0]->ptr);
    redisAssertWithInfo(c,NULL,c->cmd != NULL);
    va_end(ap);
}

/* Rewrite a single item in the command vector.
 * The new val ref count is incremented, and the old decremented. */
void rewriteClientCommandArgument(redisClient *c, int i, robj *newval) {
    robj *oldval;

    redisAssertWithInfo(c,NULL,i < c->argc);
    oldval = c->argv[i];
    c->argv[i] = newval;
    incrRefCount(newval);
    decrRefCount(oldval);

    /* If this is the command name make sure to fix c->cmd. */
    if (i == 0) {
        c->cmd = lookupCommandOrOriginal(c->argv[0]->ptr);
        redisAssertWithInfo(c,NULL,c->cmd != NULL);
    }
}

/* This function returns the number of bytes that Redis is virtually
 * using to store the reply still not read by the client.
 * It is "virtual" since the reply output list may contain objects that
 * are shared and are not really using additional memory.
 *
 * The function returns the total sum of the length of all the objects
 * stored in the output list, plus the memory used to allocate every
 * list node. The static reply buffer is not taken into account since it
 * is allocated anyway.
 *
 * Note: this function is very fast so can be called as many time as
 * the caller wishes. The main usage of this function currently is
 * enforcing the client output length limits. */
unsigned long getClientOutputBufferMemoryUsage(redisClient *c) {
    unsigned long list_item_size = sizeof(listNode)+sizeof(robj);

    return c->reply_bytes + (list_item_size*listLength(c->reply));
}

/* Get the class of a client, used in order to enforce limits to different
 * classes of clients.
 *
 * The function will return one of the following:
 * REDIS_CLIENT_TYPE_NORMAL -> Normal client
 * REDIS_CLIENT_TYPE_SLAVE  -> Slave or client executing MONITOR command
 * REDIS_CLIENT_TYPE_PUBSUB -> Client subscribed to Pub/Sub channels
 */
int getClientType(redisClient *c) {
    if ((c->flags & REDIS_SLAVE) && !(c->flags & REDIS_MONITOR))
        return REDIS_CLIENT_TYPE_SLAVE;
    if (c->flags & REDIS_PUBSUB)
        return REDIS_CLIENT_TYPE_PUBSUB;
    return REDIS_CLIENT_TYPE_NORMAL;
}

int getClientTypeByName(char *name) {
    if (!strcasecmp(name,"normal")) return REDIS_CLIENT_TYPE_NORMAL;
    else if (!strcasecmp(name,"slave")) return REDIS_CLIENT_TYPE_SLAVE;
    else if (!strcasecmp(name,"pubsub")) return REDIS_CLIENT_TYPE_PUBSUB;
    else return -1;
}

char *getClientTypeName(int class) {
    switch(class) {
    case REDIS_CLIENT_TYPE_NORMAL: return "normal";
    case REDIS_CLIENT_TYPE_SLAVE:  return "slave";
    case REDIS_CLIENT_TYPE_PUBSUB: return "pubsub";
    default:                       return NULL;
    }
}

/* The function checks if the client reached output buffer soft or hard
 * limit, and also update the state needed to check the soft limit as
 * a side effect.
 *
 * Return value: non-zero if the client reached the soft or the hard limit.
 *               Otherwise zero is returned. */
int checkClientOutputBufferLimits(redisClient *c) {
    int soft = 0, hard = 0, class;
    unsigned long used_mem = getClientOutputBufferMemoryUsage(c);

    class = getClientType(c);
    if (server.client_obuf_limits[class].hard_limit_bytes &&
        used_mem >= server.client_obuf_limits[class].hard_limit_bytes)
        hard = 1;
    if (server.client_obuf_limits[class].soft_limit_bytes &&
        used_mem >= server.client_obuf_limits[class].soft_limit_bytes)
        soft = 1;

    /* We need to check if the soft limit is reached continuously for the
     * specified amount of seconds. */
    if (soft) {
        if (c->obuf_soft_limit_reached_time == 0) {
            c->obuf_soft_limit_reached_time = server.unixtime;
            soft = 0; /* First time we see the soft limit reached */
        } else {
            time_t elapsed = server.unixtime - c->obuf_soft_limit_reached_time;

            if (elapsed <=
                server.client_obuf_limits[class].soft_limit_seconds) {
                soft = 0; /* The client still did not reached the max number of
                             seconds for the soft limit to be considered
                             reached. */
            }
        }
    } else {
        c->obuf_soft_limit_reached_time = 0;
    }
    return soft || hard;
}

/* Asynchronously close a client if soft or hard limit is reached on the
 * output buffer size. The caller can check if the client will be closed
 * checking if the client REDIS_CLOSE_ASAP flag is set.
 *
 * Note: we need to close the client asynchronously because this function is
 * called from contexts where the client can't be freed safely, i.e. from the
 * lower level functions pushing data inside the client output buffers. */

//---------------------------------------------------------------------
// 检测发送缓冲区大小是否超过限制，超过就异步断开客户端连接
//
// 不能直接销毁客户端是因为此时可能正在不停的将响应添加到发送缓冲区中
//---------------------------------------------------------------------
void asyncCloseClientOnOutputBufferLimitReached(redisClient *c) {
    redisAssert(c->reply_bytes < ULONG_MAX-(1024*64));
    // 发送缓冲区为空或者已设置关闭标志时直接返回
    if (c->reply_bytes == 0 || c->flags & REDIS_CLOSE_ASAP) return;
    if (checkClientOutputBufferLimits(c)) {
        sds client = catClientInfoString(sdsempty(),c);

        // 异步销毁客户端连接
        freeClientAsync(c);
        redisLog(REDIS_WARNING,"Client %s scheduled to be closed ASAP for overcoming of output buffer limits.", client);
        sdsfree(client);
    }
}

/* Helper function used by freeMemoryIfNeeded() in order to flush slaves
 * output buffers without returning control to the event loop. */
void flushSlavesOutputBuffers(void) {
    listIter li;
    listNode *ln;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = listNodeValue(ln);
        int events;

        /* Note that the following will not flush output buffers of slaves
         * in STATE_ONLINE but having put_online_on_ack set to true: in this
         * case the writable event is never installed, since the purpose
         * of put_online_on_ack is to postpone the moment it is installed.
         * This is what we want since slaves in this state should not receive
         * writes before the first ACK. */
        events = aeGetFileEvents(server.el,slave->fd);
        if (events & AE_WRITABLE &&
            slave->replstate == REDIS_REPL_ONLINE &&
            listLength(slave->reply))
        {
            sendReplyToClient(server.el,slave->fd,slave,0);
        }
    }
}

/* This function is called by Redis in order to process a few events from
 * time to time while blocked into some not interruptible operation.
 * This allows to reply to clients with the -LOADING error while loading the
 * data set at startup or after a full resynchronization with the master
 * and so forth.
 *
 * It calls the event loop in order to process a few events. Specifically we
 * try to call the event loop for times as long as we receive acknowledge that
 * some event was processed, in order to go forward with the accept, read,
 * write, close sequence needed to serve a client.
 *
 * The function returns the total number of events processed. */
int processEventsWhileBlocked(void) {
    int iterations = 4; /* See the function top-comment. */
    int count = 0;
    while (iterations--) {
        int events = aeProcessEvents(server.el, AE_FILE_EVENTS|AE_DONT_WAIT);
        if (!events) break;
        count += events;
    }
    return count;
}
