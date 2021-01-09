/* Select()-based ae.c module.
 *
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


#include <string.h>

//---------------------------------------------------------------------
// eventLoop->apidata
//---------------------------------------------------------------------
typedef struct aeApiState {
    fd_set rfds, wfds;
    /* We need to have a copy of the fd sets as it's not safe to reuse
     * FD sets after select(). */
    // 因为 select 会修改 rfds,wfds，所以每次 select 时拷贝一份
    fd_set _rfds, _wfds;
} aeApiState;


//---------------------------------------------------------------------
// 创建 aeApiState
//---------------------------------------------------------------------
static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));

    if (!state) return -1;
    // 初始化 rfds,wfds
    FD_ZERO(&state->rfds);
    FD_ZERO(&state->wfds);
    eventLoop->apidata = state;
    return 0;
}

//---------------------------------------------------------------------
// 调整事件集合大小
//---------------------------------------------------------------------
static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    /* Just ensure we have enough room in the fd_set type. */
    // 不能超过 FD_SETSIZE
    if (setsize >= FD_SETSIZE) return -1;
    return 0;
}

//---------------------------------------------------------------------
// 释放 eventLoop->apidata
//---------------------------------------------------------------------
static void aeApiFree(aeEventLoop *eventLoop) {
    zfree(eventLoop->apidata);
}

//---------------------------------------------------------------------
// 添加 fd 事件监听
//---------------------------------------------------------------------
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;

    // 可读就加到 rfds 中
    if (mask & AE_READABLE) FD_SET(fd,&state->rfds);
    // 可写就加到 rfds 中
    if (mask & AE_WRITABLE) FD_SET(fd,&state->wfds);
    return 0;
}

//---------------------------------------------------------------------
// 移除 fd 上的指定事件监听
//---------------------------------------------------------------------
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;

    // 可读就从 rfds 中移除
    if (mask & AE_READABLE) FD_CLR(fd,&state->rfds);
    // 可写就从 rfds 中移除
    if (mask & AE_WRITABLE) FD_CLR(fd,&state->wfds);
}

//---------------------------------------------------------------------
// 执行 poll
//---------------------------------------------------------------------
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, j, numevents = 0;

    // 拷贝 rfds,wfds 到 _rfds,_wfds
    memcpy(&state->_rfds,&state->rfds,sizeof(fd_set));
    memcpy(&state->_wfds,&state->wfds,sizeof(fd_set));

    // select
    retval = select(eventLoop->maxfd+1,
                &state->_rfds,&state->_wfds,NULL,tvp);
    if (retval > 0) {
        for (j = 0; j <= eventLoop->maxfd; j++) {
            int mask = 0;
            aeFileEvent *fe = &eventLoop->events[j];

            // 没有可读可写事件的跳过
            if (fe->mask == AE_NONE) continue;
            // 转换 mask
            if (fe->mask & AE_READABLE && FD_ISSET(j,&state->_rfds))
                mask |= AE_READABLE;
            if (fe->mask & AE_WRITABLE && FD_ISSET(j,&state->_wfds))
                mask |= AE_WRITABLE;
            // 拷贝 （fd, mask) 到 eventLoop->fired 中
            eventLoop->fired[numevents].fd = j;
            eventLoop->fired[numevents].mask = mask;
            numevents++;
        }
    }
    return numevents;
}

//---------------------------------------------------------------------
// 返回接口实现名称
//---------------------------------------------------------------------
static char *aeApiName(void) {
    return "select";
}
