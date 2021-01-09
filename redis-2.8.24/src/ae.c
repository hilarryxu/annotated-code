/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "ae.h"
#include "zmalloc.h"
#include "config.h"

/* Include the best multiplexing layer supported by this system.
 * The following should be ordered by performances, descending. */

//---------------------------------------------------------------------
// 根据配置选择最合适的后端实现
//---------------------------------------------------------------------
#ifdef HAVE_EVPORT
#include "ae_evport.c"
#else
    #ifdef HAVE_EPOLL
    #include "ae_epoll.c"
    #else
        #ifdef HAVE_KQUEUE
        #include "ae_kqueue.c"
        #else
        #include "ae_select.c"
        #endif
    #endif
#endif


//---------------------------------------------------------------------
// 创建 ioloop
//---------------------------------------------------------------------
aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;
    int i;

    if ((eventLoop = zmalloc(sizeof(*eventLoop))) == NULL) goto err;
    eventLoop->events = zmalloc(sizeof(aeFileEvent)*setsize);
    eventLoop->fired = zmalloc(sizeof(aeFiredEvent)*setsize);
    if (eventLoop->events == NULL || eventLoop->fired == NULL) goto err;

    eventLoop->setsize = setsize;
    // 记录当前时间
    eventLoop->lastTime = time(NULL);
    eventLoop->timeEventHead = NULL;
    // timer_id 置为 0
    eventLoop->timeEventNextId = 0;
    eventLoop->stop = 0;
    eventLoop->maxfd = -1;
    eventLoop->beforesleep = NULL;

    // 创建底层的 apidata
    if (aeApiCreate(eventLoop) == -1) goto err;

    /* Events with mask == AE_NONE are not set. So let's initialize the
     * vector with it. */
    // 初始化事件数组的 mask = AE_NONE
    for (i = 0; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    // 返回 eventLoop
    return eventLoop;

err:
    if (eventLoop) {
        zfree(eventLoop->events);
        zfree(eventLoop->fired);
        zfree(eventLoop);
    }
    return NULL;
}

/* Return the current set size. */

//---------------------------------------------------------------------
// 设置要监听的事件个数
//---------------------------------------------------------------------
int aeGetSetSize(aeEventLoop *eventLoop) {
    return eventLoop->setsize;
}

/* Resize the maximum set size of the event loop.
 * If the requested set size is smaller than the current set size, but
 * there is already a file descriptor in use that is >= the requested
 * set size minus one, AE_ERR is returned and the operation is not
 * performed at all.
 *
 * Otherwise AE_OK is returned and the operation is successful. */

//---------------------------------------------------------------------
// 调整要监听事件集合大小
//
// 调用该函数会将现有已注册文件描述符事件监听清空
//---------------------------------------------------------------------
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize) {
    int i;

    if (setsize == eventLoop->setsize) return AE_OK;
    // maxfd 和 setsize 大小不匹配
    if (eventLoop->maxfd >= setsize) return AE_ERR;
    // 调用底层的 aeApiResize
    if (aeApiResize(eventLoop,setsize) == -1) return AE_ERR;

    // 重新分配相应内存
    eventLoop->events = zrealloc(eventLoop->events,sizeof(aeFileEvent)*setsize);
    eventLoop->fired = zrealloc(eventLoop->fired,sizeof(aeFiredEvent)*setsize);
    eventLoop->setsize = setsize;

    /* Make sure that if we created new slots, they are initialized with
     * an AE_NONE mask. */
    // 掩码初始化为 AE_NONE
    for (i = eventLoop->maxfd+1; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    return AE_OK;
}


//---------------------------------------------------------------------
// 释放 ioloop
//---------------------------------------------------------------------
void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    aeApiFree(eventLoop);
    zfree(eventLoop->events);
    zfree(eventLoop->fired);
    zfree(eventLoop);
}


//---------------------------------------------------------------------
// 停止 ioloop
//---------------------------------------------------------------------
void aeStop(aeEventLoop *eventLoop) {
    eventLoop->stop = 1;
}


//---------------------------------------------------------------------
// 创建文件描述符监听
//---------------------------------------------------------------------
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData)
{
    // fd 的值越界，超出范围
    if (fd >= eventLoop->setsize) {
        errno = ERANGE;
        return AE_ERR;
    }
    aeFileEvent *fe = &eventLoop->events[fd];

    // 调用具体实现 aeApiAddEvent
    if (aeApiAddEvent(eventLoop, fd, mask) == -1)
        return AE_ERR;
    // 和旧的事件掩码合并
    fe->mask |= mask;
    // 读写共用同一个回调函数
    if (mask & AE_READABLE) fe->rfileProc = proc;
    if (mask & AE_WRITABLE) fe->wfileProc = proc;
    fe->clientData = clientData;
    // 更新最大的文件描述符 maxfd
    if (fd > eventLoop->maxfd)
        eventLoop->maxfd = fd;
    return AE_OK;
}

//---------------------------------------------------------------------
// 移除文件描述符监听
//---------------------------------------------------------------------
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask)
{
    if (fd >= eventLoop->setsize) return;
    aeFileEvent *fe = &eventLoop->events[fd];
    // AE_NONE 时直接跳过
    if (fe->mask == AE_NONE) return;

    // 调用具体实现 aeApiDelEvent
    aeApiDelEvent(eventLoop, fd, mask);
    // 更新事件掩码
    fe->mask = fe->mask & (~mask);

    // 如果 fd 为 maxfd，且 mask == AE_NONE
    // 就由后往前寻找新的最大 fd
    if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
        /* Update the max fd */
        int j;

        for (j = eventLoop->maxfd-1; j >= 0; j--)
            if (eventLoop->events[j].mask != AE_NONE) break;
        eventLoop->maxfd = j;
    }
}


//---------------------------------------------------------------------
// 获取 fd 对应的监听事件掩码
//---------------------------------------------------------------------
int aeGetFileEvents(aeEventLoop *eventLoop, int fd) {
    if (fd >= eventLoop->setsize) return 0;
    aeFileEvent *fe = &eventLoop->events[fd];

    return fe->mask;
}


//---------------------------------------------------------------------
// 获取当前时间（微妙级别）
//---------------------------------------------------------------------
static void aeGetTime(long *seconds, long *milliseconds)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    *seconds = tv.tv_sec;
    *milliseconds = tv.tv_usec/1000;
}


//---------------------------------------------------------------------
// 计算当前时间 + 多少毫秒
//---------------------------------------------------------------------
static void aeAddMillisecondsToNow(long long milliseconds, long *sec, long *ms) {
    long cur_sec, cur_ms, when_sec, when_ms;

    aeGetTime(&cur_sec, &cur_ms);
    when_sec = cur_sec + milliseconds/1000;
    when_ms = cur_ms + milliseconds%1000;
    if (when_ms >= 1000) {
        when_sec ++;
        when_ms -= 1000;
    }
    *sec = when_sec;
    *ms = when_ms;
}


//---------------------------------------------------------------------
// 创建定时器事件
//---------------------------------------------------------------------
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc)
{
    // 从 1 开始递增
    long long id = eventLoop->timeEventNextId++;
    aeTimeEvent *te;

    te = zmalloc(sizeof(*te));
    if (te == NULL) return AE_ERR;
    te->id = id;
    // 计算绝对时间
    aeAddMillisecondsToNow(milliseconds,&te->when_sec,&te->when_ms);
    te->timeProc = proc;
    te->finalizerProc = finalizerProc;
    te->clientData = clientData;
    // 头插法
    te->next = eventLoop->timeEventHead;
    eventLoop->timeEventHead = te;
    return id;
}

//---------------------------------------------------------------------
// 删除定时器事件
//---------------------------------------------------------------------
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id)
{
    aeTimeEvent *te, *prev = NULL;

    te = eventLoop->timeEventHead;
    // 遍历定时器链表
    while(te) {
        if (te->id == id) {
            // 第一个节点时特殊处理
            if (prev == NULL)
                eventLoop->timeEventHead = te->next;
            else
                // 从链表中移除
                prev->next = te->next;
            // 执行销毁回调函数
            if (te->finalizerProc)
                te->finalizerProc(eventLoop, te->clientData);
            zfree(te);
            return AE_OK;
        }
        // 记录前驱节点，删除当前节点时要用到
        prev = te;
        te = te->next;
    }

    // 没找到 timer id 对应的定时器
    return AE_ERR; /* NO event with the specified ID found */
}

/* Search the first timer to fire.
 * This operation is useful to know how many time the select can be
 * put in sleep without to delay any event.
 * If there are no timers NULL is returned.
 *
 * Note that's O(N) since time events are unsorted.
 * Possible optimizations (not needed by Redis so far, but...):
 * 1) Insert the event in order, so that the nearest is just the head.
 *    Much better but still insertion or deletion of timers is O(N).
 * 2) Use a skiplist to have this operation as O(1) and insertion as O(log(N)).
 */

//---------------------------------------------------------------------
// 找出最早要到期的那个定时器，算法复杂度为 O(N)
//
// 因为 redis 用到的定时器比较少，所以暂时无需优化这一块
//---------------------------------------------------------------------
static aeTimeEvent *aeSearchNearestTimer(aeEventLoop *eventLoop)
{
    aeTimeEvent *te = eventLoop->timeEventHead;
    aeTimeEvent *nearest = NULL;

    while(te) {
        if (!nearest || te->when_sec < nearest->when_sec ||
                (te->when_sec == nearest->when_sec &&
                 te->when_ms < nearest->when_ms))
            // 更新 nearest
            nearest = te;
        te = te->next;
    }
    return nearest;
}

/* Process time events */

//---------------------------------------------------------------------
// 处理定时器事件
//
// 返回值为：处理事件的个数
//---------------------------------------------------------------------
static int processTimeEvents(aeEventLoop *eventLoop) {
    int processed = 0;
    aeTimeEvent *te;
    long long maxId;
    time_t now = time(NULL);

    /* If the system clock is moved to the future, and then set back to the
     * right value, time events may be delayed in a random way. Often this
     * means that scheduled operations will not be performed soon enough.
     *
     * Here we try to detect system clock skews, and force all the time
     * events to be processed ASAP when this happens: the idea is that
     * processing events earlier is less dangerous than delaying them
     * indefinitely, and practice suggests it is. */
    // 系统时间回跳了
    if (now < eventLoop->lastTime) {
        // 此时所用定时器都要执行
        te = eventLoop->timeEventHead;
        while(te) {
            te->when_sec = 0;
            te = te->next;
        }
    }
    // 更新 lastTime
    eventLoop->lastTime = now;

    te = eventLoop->timeEventHead;
    // maxId 为当前最大 timer id
    maxId = eventLoop->timeEventNextId-1;
    // 遍历链表
    while(te) {
        long now_sec, now_ms;
        long long id;

        // 定时器处理过程中可能新添加了定时器，也可能删除定时器
        // 定时器链表会改变
        // 新添加的定时器 id 会比上面保存的 maxId 要大
        if (te->id > maxId) {
            // 跳过这些新添加的定时器（下次 ioloop 循环再处理）
            te = te->next;
            continue;
        }
        // 循环中调用这个可能有性能问题
        aeGetTime(&now_sec, &now_ms);
        // 判断定时器时间到了
        if (now_sec > te->when_sec ||
            (now_sec == te->when_sec && now_ms >= te->when_ms))
        {
            int retval;

            // 保存 timer id
            id = te->id;
            // 调用定时器回调函数
            retval = te->timeProc(eventLoop, id, te->clientData);
            processed++;
            /* After an event is processed our time event list may
             * no longer be the same, so we restart from head.
             * Still we make sure to don't process events registered
             * by event handlers itself in order to don't loop forever.
             * To do so we saved the max ID we want to handle.
             *
             * FUTURE OPTIMIZATIONS:
             * Note that this is NOT great algorithmically. Redis uses
             * a single time event so it's not a problem but the right
             * way to do this is to add the new elements on head, and
             * to flag deleted elements in a special way for later
             * deletion (putting references to the nodes to delete into
             * another linked list). */
            if (retval != AE_NOMORE) {
                // 重复循环的定时器
                // 更新定时器事件信息
                aeAddMillisecondsToNow(retval,&te->when_sec,&te->when_ms);
            } else {
                // 一次性定时器
                // 从 ioloop 中移除
                aeDeleteTimeEvent(eventLoop, id);
            }

            // 从链表头部开始重新遍历（因为链表可能有变化）
            te = eventLoop->timeEventHead;
        } else {
            te = te->next;
        }
    }

    // 返回已处理事件数
    return processed;
}

/* Process every pending time event, then every pending file event
 * (that may be registered by time event callbacks just processed).
 * Without special flags the function sleeps until some file event
 * fires, or when the next time event occurs (if any).
 *
 * If flags is 0, the function does nothing and returns.
 * if flags has AE_ALL_EVENTS set, all the kind of events are processed.
 * if flags has AE_FILE_EVENTS set, file events are processed.
 * if flags has AE_TIME_EVENTS set, time events are processed.
 * if flags has AE_DONT_WAIT set the function returns ASAP until all
 * the events that's possible to process without to wait are processed.
 *
 * The function returns the number of events processed. */

//---------------------------------------------------------------------
// 事件处理
//
// 返回值为：处理事件的个数
//---------------------------------------------------------------------
int aeProcessEvents(aeEventLoop *eventLoop, int flags)
{
    int processed = 0, numevents;

    /* Nothing to do? return ASAP */
    // flags 不正确直接返回 0
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

    /* Note that we want call select() even if there are no
     * file events to process as long as we want to process time
     * events, in order to sleep until the next time event is ready
     * to fire. */
    // 前提条件：
    //
    //   eventLoop->maxfd != -1
    //     表示有 fd 事件需要处理
    //
    //   或者 (flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT)
    //     需要处理定时器事件且需要阻塞等待文件描述符事件到来
    if (eventLoop->maxfd != -1 ||
        ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        int j;
        aeTimeEvent *shortest = NULL;
        struct timeval tv, *tvp;

        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT))
            // 计算最早要到期定时器事件
            shortest = aeSearchNearestTimer(eventLoop);
        if (shortest) {
            long now_sec, now_ms;

            /* Calculate the time missing for the nearest
             * timer to fire. */
            // 计算等下 poll 需要设置超时的时间差
            aeGetTime(&now_sec, &now_ms);
            tvp = &tv;
            tvp->tv_sec = shortest->when_sec - now_sec;
            if (shortest->when_ms < now_ms) {
                tvp->tv_usec = ((shortest->when_ms+1000) - now_ms)*1000;
                tvp->tv_sec --;
            } else {
                tvp->tv_usec = (shortest->when_ms - now_ms)*1000;
            }
            if (tvp->tv_sec < 0) tvp->tv_sec = 0;
            if (tvp->tv_usec < 0) tvp->tv_usec = 0;
        } else {
            // 没有定时器事件的情况
            /* If we have to check for events but need to return
             * ASAP because of AE_DONT_WAIT we need to set the timeout
             * to zero */
            // AE_DONT_WAIT 迅速 poll 一次
            if (flags & AE_DONT_WAIT) {
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            } else {
                // 阻塞等待事件到来
                /* Otherwise we can block */
                tvp = NULL; /* wait forever */
            }
        }

        // 执行底层的 poll
        numevents = aeApiPoll(eventLoop, tvp);
        for (j = 0; j < numevents; j++) {
            // 定位 fd 事件结构体
            aeFileEvent *fe = &eventLoop->events[eventLoop->fired[j].fd];
            // 读取触发的 (fd, mask)
            int mask = eventLoop->fired[j].mask;
            int fd = eventLoop->fired[j].fd;
            int rfired = 0;

            /* note the fe->mask & mask & ... code: maybe an already processed
             * event removed an element that fired and we still didn't
             * processed, so we check if the event is still valid. */
            // 这种检测方式更加严格
            if (fe->mask & mask & AE_READABLE) {
                // 读事件触发标记
                rfired = 1;
                // 执行可读事件回调函数
                fe->rfileProc(eventLoop,fd,fe->clientData,mask);
            }
            if (fe->mask & mask & AE_WRITABLE) {
                // 读写回调相同时，直接调用可写回调函数（默认情况）
                // 不同时，且可写事件触发时同时没有触发可读事件，才调用可写回调函数
                if (!rfired || fe->wfileProc != fe->rfileProc)
                    fe->wfileProc(eventLoop,fd,fe->clientData,mask);
            }
            // 更新以处理事件计数
            processed++;
        }
    }


    /* Check time events */
    // 处理定时器事件
    if (flags & AE_TIME_EVENTS)
        processed += processTimeEvents(eventLoop);

    return processed; /* return the number of processed file/time events */
}

/* Wait for milliseconds until the given file descriptor becomes
 * writable/readable/exception */

//---------------------------------------------------------------------
// 带超时的检测 fd 监听事件
//---------------------------------------------------------------------
int aeWait(int fd, int mask, long long milliseconds) {
    struct pollfd pfd;
    int retmask = 0, retval;

    memset(&pfd, 0, sizeof(pfd));
    // 设置 (fd, mask)
    pfd.fd = fd;
    if (mask & AE_READABLE) pfd.events |= POLLIN;
    if (mask & AE_WRITABLE) pfd.events |= POLLOUT;

    if ((retval = poll(&pfd, 1, milliseconds))== 1) {
        // poll 返回 1，表示有事件触发
        if (pfd.revents & POLLIN) retmask |= AE_READABLE;
        if (pfd.revents & POLLOUT) retmask |= AE_WRITABLE;

        // POLLERR,POLLHUP -> AE_WRITABLE
        if (pfd.revents & POLLERR) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLHUP) retmask |= AE_WRITABLE;
        // 返回掩码
        return retmask;
    } else {
        // 0 表示超时
        // -1 表示出错
        return retval;
    }
}


//---------------------------------------------------------------------
// ioloop 主循环
//---------------------------------------------------------------------
void aeMain(aeEventLoop *eventLoop) {
    eventLoop->stop = 0;
    // 循环
    while (!eventLoop->stop) {
        // 运行 beforesleep
        if (eventLoop->beforesleep != NULL)
            eventLoop->beforesleep(eventLoop);
        // 处理 fd 事件和定时器事件
        aeProcessEvents(eventLoop, AE_ALL_EVENTS);
    }
}


//---------------------------------------------------------------------
// 返回底层实现名称
//---------------------------------------------------------------------
char *aeGetApiName(void) {
    return aeApiName();
}


//---------------------------------------------------------------------
// 设置每次轮询前要执行的函数
//---------------------------------------------------------------------
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep) {
    eventLoop->beforesleep = beforesleep;
}
