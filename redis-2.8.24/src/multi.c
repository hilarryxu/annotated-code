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

/* ================================ MULTI/EXEC ============================== */

/* Client state initialization for MULTI/EXEC */

//---------------------------------------------------------------------
// 初始化客户端的 multi 状态结构体
//---------------------------------------------------------------------
void initClientMultiState(redisClient *c) {
    // 命令队列初始化为空
    c->mstate.commands = NULL;
    c->mstate.count = 0;
}

/* Release all the resources associated with MULTI/EXEC state */

//---------------------------------------------------------------------
// 释放 multi 状态
//---------------------------------------------------------------------
void freeClientMultiState(redisClient *c) {
    int j;

    for (j = 0; j < c->mstate.count; j++) {
        int i;
        multiCmd *mc = c->mstate.commands+j;

        for (i = 0; i < mc->argc; i++)
            decrRefCount(mc->argv[i]);
        zfree(mc->argv);
    }
    zfree(c->mstate.commands);
}

/* Add a new command into the MULTI commands queue */

//---------------------------------------------------------------------
// 命令入队
//---------------------------------------------------------------------
void queueMultiCommand(redisClient *c) {
    multiCmd *mc;
    int j;

    // 每次都重新分配拷贝
    c->mstate.commands = zrealloc(c->mstate.commands,
            sizeof(multiCmd)*(c->mstate.count+1));
    mc = c->mstate.commands+c->mstate.count;
    // 拷贝当前命令的处理函数指针
    mc->cmd = c->cmd;
    // 拷贝参数个数和参数列表
    mc->argc = c->argc;
    mc->argv = zmalloc(sizeof(robj*)*c->argc);
    memcpy(mc->argv,c->argv,sizeof(robj*)*c->argc);
    for (j = 0; j < c->argc; j++)
        incrRefCount(mc->argv[j]);
    // 个数加 1
    c->mstate.count++;
}


//---------------------------------------------------------------------
// 取消事务
//---------------------------------------------------------------------
void discardTransaction(redisClient *c) {
    // 释放
    freeClientMultiState(c);
    // 重置
    initClientMultiState(c);
    // 清掉 REDIS_MULTI|REDIS_DIRTY_CAS|REDIS_DIRTY_EXEC 标志
    c->flags &= ~(REDIS_MULTI|REDIS_DIRTY_CAS|REDIS_DIRTY_EXEC);
    // 从观察键列表中移除自己
    unwatchAllKeys(c);
}

/* Flag the transacation as DIRTY_EXEC so that EXEC will fail.
 * Should be called every time there is an error while queueing a command. */
void flagTransaction(redisClient *c) {
    if (c->flags & REDIS_MULTI)
        c->flags |= REDIS_DIRTY_EXEC;
}


//---------------------------------------------------------------------
// multi 命令
//---------------------------------------------------------------------
void multiCommand(redisClient *c) {
    if (c->flags & REDIS_MULTI) {
        addReplyError(c,"MULTI calls can not be nested");
        return;
    }
    // 设置 REDIS_MULTI 标志
    c->flags |= REDIS_MULTI;
    addReply(c,shared.ok);
}


//---------------------------------------------------------------------
// discard 命令
//---------------------------------------------------------------------
void discardCommand(redisClient *c) {
    if (!(c->flags & REDIS_MULTI)) {
        addReplyError(c,"DISCARD without MULTI");
        return;
    }
    discardTransaction(c);
    addReply(c,shared.ok);
}

/* Send a MULTI command to all the slaves and AOF file. Check the execCommand
 * implementation for more information. */
void execCommandPropagateMulti(redisClient *c) {
    robj *multistring = createStringObject("MULTI",5);

    propagate(server.multiCommand,c->db->id,&multistring,1,
              REDIS_PROPAGATE_AOF|REDIS_PROPAGATE_REPL);
    decrRefCount(multistring);
}


//---------------------------------------------------------------------
// exec 命令
//---------------------------------------------------------------------
void execCommand(redisClient *c) {
    int j;
    robj **orig_argv;
    int orig_argc;
    struct redisCommand *orig_cmd;
    int must_propagate = 0; /* Need to propagate MULTI/EXEC to AOF / slaves? */

    if (!(c->flags & REDIS_MULTI)) {
        addReplyError(c,"EXEC without MULTI");
        return;
    }

    /* Check if we need to abort the EXEC because:
     * 1) Some WATCHed key was touched.
     * 2) There was a previous error while queueing commands.
     * A failed EXEC in the first case returns a multi bulk nil object
     * (technically it is not an error but a special behavior), while
     * in the second an EXECABORT error is returned. */
    // 检测 exec 无法执行的情况
    //   1. 观察的键有数据改动
    //   2. 入队时命令有错误
    if (c->flags & (REDIS_DIRTY_CAS|REDIS_DIRTY_EXEC)) {
        addReply(c, c->flags & REDIS_DIRTY_EXEC ? shared.execaborterr :
                                                  shared.nullmultibulk);
        discardTransaction(c);
        goto handle_monitor;
    }

    /* Exec all the queued commands */
    // 首先清除观察的键
    unwatchAllKeys(c); /* Unwatch ASAP otherwise we'll waste CPU cycles */
    // 保存一下当前命令相关结构
    orig_argv = c->argv;
    orig_argc = c->argc;
    orig_cmd = c->cmd;
    addReplyMultiBulkLen(c,c->mstate.count);
    // 遍历命令队列
    for (j = 0; j < c->mstate.count; j++) {
        c->argc = c->mstate.commands[j].argc;
        c->argv = c->mstate.commands[j].argv;
        c->cmd = c->mstate.commands[j].cmd;

        /* Propagate a MULTI request once we encounter the first write op.
         * This way we'll deliver the MULTI/..../EXEC block as a whole and
         * both the AOF and the replication link will have the same consistency
         * and atomicity guarantees. */
        // 判断是否许要传递 MULTI/..../EXEC 给从服务器以及 AOF 文件
        if (!must_propagate && !(c->cmd->flags & REDIS_CMD_READONLY)) {
            execCommandPropagateMulti(c);
            must_propagate = 1;
        }

        // 执行命令
        call(c,REDIS_CALL_FULL);

        /* Commands may alter argc/argv, restore mstate. */
        c->mstate.commands[j].argc = c->argc;
        c->mstate.commands[j].argv = c->argv;
        c->mstate.commands[j].cmd = c->cmd;
    }
    // 恢复当前命令相关结构
    c->argv = orig_argv;
    c->argc = orig_argc;
    c->cmd = orig_cmd;
    // 重置状态
    discardTransaction(c);
    /* Make sure the EXEC command will be propagated as well if MULTI
     * was already propagated. */
    if (must_propagate) server.dirty++;

handle_monitor:
    // 处理 monitor
    /* Send EXEC to clients waiting data from MONITOR. We do it here
     * since the natural order of commands execution is actually:
     * MUTLI, EXEC, ... commands inside transaction ...
     * Instead EXEC is flagged as REDIS_CMD_SKIP_MONITOR in the command
     * table, and we do it here with correct ordering. */
    if (listLength(server.monitors) && !server.loading)
        replicationFeedMonitors(c,server.monitors,c->db->id,c->argv,c->argc);
}

/* ===================== WATCH (CAS alike for MULTI/EXEC) ===================
 *
 * The implementation uses a per-DB hash table mapping keys to list of clients
 * WATCHing those keys, so that given a key that is going to be modified
 * we can mark all the associated clients as dirty.
 *
 * Also every client contains a list of WATCHed keys so that's possible to
 * un-watch such keys when the client is freed or when UNWATCH is called. */

/* In the client->watched_keys list we need to use watchedKey structures
 * as in order to identify a key in Redis we need both the key name and the
 * DB */

//---------------------------------------------------------------------
// (db, key)
//---------------------------------------------------------------------
typedef struct watchedKey {
    robj *key;
    redisDb *db;
} watchedKey;

/* Watch for the specified key */

//---------------------------------------------------------------------
// 观察指定的键
//---------------------------------------------------------------------
void watchForKey(redisClient *c, robj *key) {
    list *clients = NULL;
    listIter li;
    listNode *ln;
    watchedKey *wk;

    /* Check if we are already watching for this key */
    // 遍历列表，判断是否已添加了观察
    listRewind(c->watched_keys,&li);
    while((ln = listNext(&li))) {
        wk = listNodeValue(ln);
        if (wk->db == c->db && equalStringObjects(key,wk->key))
            return; /* Key already watched */
    }

    /* This key is not already watched in this DB. Let's add it */
    // db->watched_keys {
    //   "key": [client1, client2, ...]
    // }
    clients = dictFetchValue(c->db->watched_keys,key);
    if (!clients) {
        // 键不存在字典中，就新键键值对
        clients = listCreate();
        dictAdd(c->db->watched_keys,key,clients);
        incrRefCount(key);
    }
    // 添加到链表尾部
    listAddNodeTail(clients,c);

    /* Add the new key to the list of keys watched by this client */
    wk = zmalloc(sizeof(*wk));
    wk->key = key;
    // 保存客户端当前的数据库指针
    wk->db = c->db;
    // 增加 key 的引用计数
    incrRefCount(key);
    // 添加到客户端观察键链表的尾部
    listAddNodeTail(c->watched_keys,wk);
}

/* Unwatch all the keys watched by this client. To clean the EXEC dirty
 * flag is up to the caller. */

//---------------------------------------------------------------------
// 从观察键链表中移除自己
//---------------------------------------------------------------------
void unwatchAllKeys(redisClient *c) {
    listIter li;
    listNode *ln;

    if (listLength(c->watched_keys) == 0) return;
    // 遍历该客户端观察的键列表
    listRewind(c->watched_keys,&li);
    while((ln = listNext(&li))) {
        list *clients;
        watchedKey *wk;

        /* Lookup the watched key -> clients list and remove the client
         * from the list */
        // wk 观察的键
        wk = listNodeValue(ln);
        // db->watched_keys -> clients
        clients = dictFetchValue(wk->db->watched_keys, wk->key);
        redisAssertWithInfo(c,NULL,clients != NULL);
        // 从观察键链表中移除自己
        listDelNode(clients,listSearchKey(clients,c));
        /* Kill the entry at all if this was the only client */
        // 链表为空时，删除字典对于的键
        if (listLength(clients) == 0)
            dictDelete(wk->db->watched_keys, wk->key);
        /* Remove this watched key from the client->watched list */
        // 从 c->watched_keys 观察的键链表中移除该键
        listDelNode(c->watched_keys,ln);
        decrRefCount(wk->key);
        zfree(wk);
    }
}

/* "Touch" a key, so that if this key is being WATCHed by some client the
 * next EXEC will fail. */

//---------------------------------------------------------------------
// 键的值有修改，遍历设置客户端的 REDIS_DIRTY_CAS 标志位
//---------------------------------------------------------------------
void touchWatchedKey(redisDb *db, robj *key) {
    list *clients;
    listIter li;
    listNode *ln;

    if (dictSize(db->watched_keys) == 0) return;
    clients = dictFetchValue(db->watched_keys, key);
    if (!clients) return;

    /* Mark all the clients watching this key as REDIS_DIRTY_CAS */
    /* Check if we are already watching for this key */
    listRewind(clients,&li);
    while((ln = listNext(&li))) {
        redisClient *c = listNodeValue(ln);

        c->flags |= REDIS_DIRTY_CAS;
    }
}

/* On FLUSHDB or FLUSHALL all the watched keys that are present before the
 * flush but will be deleted as effect of the flushing operation should
 * be touched. "dbid" is the DB that's getting the flush. -1 if it is
 * a FLUSHALL operation (all the DBs flushed). */

//---------------------------------------------------------------------
// FLUSHDB,FLUSHALL 命令执行前，对应数据库中所有键都被删除
//
// 所有客户端对应数据库上观察的键都被修改了，设置 REDIS_DIRTY_CAS 标志位
//---------------------------------------------------------------------
void touchWatchedKeysOnFlush(int dbid) {
    listIter li1, li2;
    listNode *ln;

    /* For every client, check all the waited keys */
    listRewind(server.clients,&li1);
    while((ln = listNext(&li1))) {
        redisClient *c = listNodeValue(ln);
        listRewind(c->watched_keys,&li2);
        while((ln = listNext(&li2))) {
            watchedKey *wk = listNodeValue(ln);

            /* For every watched key matching the specified DB, if the
             * key exists, mark the client as dirty, as the key will be
             * removed. */
            if (dbid == -1 || wk->db->id == dbid) {
                if (dictFind(wk->db->dict, wk->key->ptr) != NULL)
                    c->flags |= REDIS_DIRTY_CAS;
            }
        }
    }
}


//---------------------------------------------------------------------
// watch 命令
//---------------------------------------------------------------------
void watchCommand(redisClient *c) {
    int j;

    if (c->flags & REDIS_MULTI) {
        addReplyError(c,"WATCH inside MULTI is not allowed");
        return;
    }
    // 遍历参数列表
    for (j = 1; j < c->argc; j++)
        // 添加键的观察
        watchForKey(c,c->argv[j]);
    addReply(c,shared.ok);
}


//---------------------------------------------------------------------
// unwatch 命令
//
// 只能一次性移除所有观察的键
//---------------------------------------------------------------------
void unwatchCommand(redisClient *c) {
    unwatchAllKeys(c);
    c->flags &= (~REDIS_DIRTY_CAS);
    addReply(c,shared.ok);
}
