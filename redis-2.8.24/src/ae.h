/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef __AE_H__
#define __AE_H__


//=====================================================================
// ioloop
//
// 其实现依赖于 fd 作为数组索引去获取相应信息
// 利用了程序产生的文件描述符从很小的值开始增长的特性
//=====================================================================

//---------------------------------------------------------------------
// 函数返回状态码
//
// 0  为成功
// -1 为失败
//---------------------------------------------------------------------
#define AE_OK 0
#define AE_ERR -1


//---------------------------------------------------------------------
// 监听事件类型
//---------------------------------------------------------------------
#define AE_NONE 0
#define AE_READABLE 1
#define AE_WRITABLE 2


//---------------------------------------------------------------------
// 事件类型
//
// 1 fd
// 2 定时器
//---------------------------------------------------------------------
#define AE_FILE_EVENTS 1
#define AE_TIME_EVENTS 2
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS)
// 不阻塞等待 poll
#define AE_DONT_WAIT 4

// 定时器回调返回该值表示是一次性定时器
#define AE_NOMORE -1

/* Macros */
#define AE_NOTUSED(V) ((void) V)

struct aeEventLoop;

/* Types and data structures */
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);

/* File event structure */

//---------------------------------------------------------------------
// fd 事件
//---------------------------------------------------------------------
typedef struct aeFileEvent {
    // 触发的事件状态
    int mask; /* one of AE_(READABLE|WRITABLE) */
    // 可读事件回调函数
    aeFileProc *rfileProc;
    // 可写事件回调函数
    aeFileProc *wfileProc;
    // 附加上下文
    void *clientData;
} aeFileEvent;

/* Time event structure */

//---------------------------------------------------------------------
// 定时器事件
//---------------------------------------------------------------------
typedef struct aeTimeEvent {
    // 定时器 ID
    long long id; /* time event identifier. */
    // 时间
    long when_sec; /* seconds */
    long when_ms; /* milliseconds */
    // 定时器回调函数
    aeTimeProc *timeProc;
    // 移除时的销毁清理函数
    aeEventFinalizerProc *finalizerProc;
    // 附加上下文
    void *clientData;
    // 单向链表 next 指针
    struct aeTimeEvent *next;
} aeTimeEvent;

/* A fired event */

//---------------------------------------------------------------------
// poll 返回的激活的事件
//---------------------------------------------------------------------
typedef struct aeFiredEvent {
    // fd
    int fd;
    // 触发的事件状态
    int mask;
} aeFiredEvent;

/* State of an event based program */

//---------------------------------------------------------------------
// ioloop 结构
//---------------------------------------------------------------------
typedef struct aeEventLoop {
    // 当前所有注册文件描述符的最大值
    int maxfd;   /* highest file descriptor currently registered */
    // 跟踪监听的文件描述符个数
    int setsize; /* max number of file descriptors tracked */
    // 下一个 timer id
    long long timeEventNextId;
    // 记录上一次定时器处理时间
    time_t lastTime;     /* Used to detect system clock skew */
    // 已注册的 fd 事件数组
    aeFileEvent *events; /* Registered events */
    // 激活的事件数组
    aeFiredEvent *fired; /* Fired events */
    // 定时器列表头指针
    aeTimeEvent *timeEventHead;
    // 停止标志
    int stop;
    // 具体实现结构体
    void *apidata; /* This is used for polling API specific data */
    // 每轮轮询前要运行的函数
    aeBeforeSleepProc *beforesleep;
} aeEventLoop;

/* Prototypes */
aeEventLoop *aeCreateEventLoop(int setsize);
void aeDeleteEventLoop(aeEventLoop *eventLoop);
void aeStop(aeEventLoop *eventLoop);
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData);
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc);
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);
int aeProcessEvents(aeEventLoop *eventLoop, int flags);
int aeWait(int fd, int mask, long long milliseconds);
void aeMain(aeEventLoop *eventLoop);
char *aeGetApiName(void);
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);
int aeGetSetSize(aeEventLoop *eventLoop);
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);

#endif
