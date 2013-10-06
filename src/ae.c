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

/*setsize 为server.maxclients+1024，也就是最大客户端连接数，猜测是一个连接一个。
这里的作用是: 初始化epoll_create，初始化fired,events等事件结构。*/
aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;
    int i;

    if ((eventLoop = zmalloc(sizeof(*eventLoop))) == NULL) goto err;
    eventLoop->events = zmalloc(sizeof(aeFileEvent)*setsize);
    eventLoop->fired = zmalloc(sizeof(aeFiredEvent)*setsize);
    if (eventLoop->events == NULL || eventLoop->fired == NULL) goto err;
    eventLoop->setsize = setsize;
    eventLoop->lastTime = time(NULL);
    eventLoop->timeEventHead = NULL;
    eventLoop->timeEventNextId = 0;
    eventLoop->stop = 0;
    eventLoop->maxfd = -1;
    eventLoop->beforesleep = NULL;
    if (aeApiCreate(eventLoop) == -1) goto err;//调用epoll_create一个句柄，分配wait时的返回事件集合数组。
    /* Events with mask == AE_NONE are not set. So let's initialize the
     * vector with it. */
    for (i = 0; i < setsize; i++)//标记所有的setsize槽位为空，也就是这个槽位不在epoll的监听事件集合里面。
        eventLoop->events[i].mask = AE_NONE;
    return eventLoop;

err:
    if (eventLoop) {
        zfree(eventLoop->events);
        zfree(eventLoop->fired);
        zfree(eventLoop);
    }
    return NULL;
}

void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    aeApiFree(eventLoop);
    zfree(eventLoop->events);
    zfree(eventLoop->fired);
    zfree(eventLoop);
}

void aeStop(aeEventLoop *eventLoop) {
    eventLoop->stop = 1;
}

int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,  aeFileProc *proc, void *clientData)
{//将参数fd加入epoll中，并保存相关回调，数据到eventLoop->events[fd]中。到时候可以根据fd取得数据。
//proc 为acceptTcpHandler 或者如果是unix域就是acceptUnixHandler
//客户端连接会设置为readQueryFromClient，用来读取客户端的query.
    if (fd >= eventLoop->setsize) {
        errno = ERANGE;
        return AE_ERR;
    }
    aeFileEvent *fe = &eventLoop->events[fd];//直接拿fd当下标获取数组。

    if (aeApiAddEvent(eventLoop, fd, mask) == -1)
        return AE_ERR;
    fe->mask |= mask;
    if (mask & AE_READABLE) fe->rfileProc = proc;
    if (mask & AE_WRITABLE) fe->wfileProc = proc;
    fe->clientData = clientData;
    if (fd > eventLoop->maxfd)
        eventLoop->maxfd = fd;
    return AE_OK;
}

void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask)
{
    if (fd >= eventLoop->setsize) return;
    aeFileEvent *fe = &eventLoop->events[fd];

    if (fe->mask == AE_NONE) return;
    fe->mask = fe->mask & (~mask);
    if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
        /* Update the max fd */
        int j;

        for (j = eventLoop->maxfd-1; j >= 0; j--)
            if (eventLoop->events[j].mask != AE_NONE) break;
        eventLoop->maxfd = j;
    }
    aeApiDelEvent(eventLoop, fd, mask);
}

int aeGetFileEvents(aeEventLoop *eventLoop, int fd) {
    if (fd >= eventLoop->setsize) return 0;
    aeFileEvent *fe = &eventLoop->events[fd];

    return fe->mask;
}

static void aeGetTime(long *seconds, long *milliseconds)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    *seconds = tv.tv_sec;
    *milliseconds = tv.tv_usec/1000;
}

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

long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
	aeTimeProc *proc, void *clientData, aeEventFinalizerProc *finalizerProc)
{//在定时器事件链表中增加一个定时器。貌似比nginx性能差多了。
    long long id = eventLoop->timeEventNextId++;
    aeTimeEvent *te;

    te = zmalloc(sizeof(*te));
    if (te == NULL) return AE_ERR;
    te->id = id;
    aeAddMillisecondsToNow(milliseconds,&te->when_sec,&te->when_ms);
    te->timeProc = proc;
    te->finalizerProc = finalizerProc;
    te->clientData = clientData;
    te->next = eventLoop->timeEventHead;//链接到链表头部。
    eventLoop->timeEventHead = te;
    return id;
}

int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id)
{//删除一个定时器事件，回调其finalizerProc如果有的话。
    aeTimeEvent *te, *prev = NULL;

    te = eventLoop->timeEventHead;
    while(te) {
        if (te->id == id) {
            if (prev == NULL)
                eventLoop->timeEventHead = te->next;
            else
                prev->next = te->next;
            if (te->finalizerProc)
                te->finalizerProc(eventLoop, te->clientData);
            zfree(te);
            return AE_OK;
        }
        prev = te;
        te = te->next;
    }
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
static aeTimeEvent *aeSearchNearestTimer(aeEventLoop *eventLoop)
{
    aeTimeEvent *te = eventLoop->timeEventHead;
    aeTimeEvent *nearest = NULL;
//扫描一遍eventLoop->timeEventHead定时器列表，找到其中最快触发的定时器。返回其指针。
    while(te) {
        if (!nearest || te->when_sec < nearest->when_sec || (te->when_sec == nearest->when_sec && te->when_ms < nearest->when_ms))
            nearest = te;
        te = te->next;
    }
    return nearest;
}

/* Process time events */
static int processTimeEvents(aeEventLoop *eventLoop) {
//循环判断eventLoop->timeEventHead链表的事件是否到达时间，如果到了就调用其timeProc回调。
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
    if (now < eventLoop->lastTime) {
        te = eventLoop->timeEventHead;
        while(te) {
            te->when_sec = 0;//强制设置超时时间为0秒
            te = te->next;
        }
    }
	//如上面注释所言，这里记录上一次进来的时间，用来判断是否系统时间被手动修改过。
	//如果手动修改过时间，并且修改的时间调后了，比如调为昨天的时间了，
	//那么这里强制把所有定时器的触发时间设为0而触发之。理由为: 早运行定时器总比往后延迟一天要好。
    eventLoop->lastTime = now;

    te = eventLoop->timeEventHead;
    maxId = eventLoop->timeEventNextId-1;
    while(te) {
        long now_sec, now_ms;
        long long id;

        if (te->id > maxId) {//id大于最大的，肯定不是有效的。
            te = te->next;
            continue;
        }
		//这个获取时间的函数在循环里面，是为了避免某个回调函数占用太多时间吧，那你怎么也处理不了之前的事件。
		//放外面应该也可以。不对，看下面if里面最后一行，te = eventLoop->timeEventHead;
		//也就是只要处理了一个定时器，那么重新从头开始处理。
        aeGetTime(&now_sec, &now_ms);
        if (now_sec > te->when_sec ||
            (now_sec == te->when_sec && now_ms >= te->when_ms))
        {//超时了，回调其函数。用te->clientData参数。
            int retval;

            id = te->id;
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
				//注意返回的retval如果部位NOMORE，那么就等于下一次触发的事件距离。milliseconds
                aeAddMillisecondsToNow(retval,&te->when_sec,&te->when_ms);
            } else {//不需要了，删除之
                aeDeleteTimeEvent(eventLoop, id);
            }
            te = eventLoop->timeEventHead;
        } else {
            te = te->next;
        }
    }
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
int aeProcessEvents(aeEventLoop *eventLoop, int flags)
{
    int processed = 0, numevents;

    /* Nothing to do? return ASAP */
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

    /* Note that we want call select() even if there are no
     * file events to process as long as we want to process time
     * events, in order to sleep until the next time event is ready
     * to fire. */
    if (eventLoop->maxfd != -1 || ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
		//如果有fd，或者(有定时器事件并且没有DONT_WAIT不要等待标志)
        int j;
        aeTimeEvent *shortest = NULL;
        struct timeval tv, *tvp;

		
		//扫描一遍eventLoop->timeEventHead定时器列表，找到其中最快触发的定时器。返回其指针。
        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT))//有定时器标志，需要扫描一下。
            shortest = aeSearchNearestTimer(eventLoop);
        if (shortest) {//找到了一个最近应该触发的定时器。
            long now_sec, now_ms;

            /* Calculate the time missing for the nearest
             * timer to fire. */
            aeGetTime(&now_sec, &now_ms);//简单gettimeofday获取当前时间
            tvp = &tv;
            tvp->tv_sec = shortest->when_sec - now_sec;//计算还要多久才能触发定时器
            if (shortest->when_ms < now_ms) {
                tvp->tv_usec = ((shortest->when_ms+1000) - now_ms)*1000;
                tvp->tv_sec --;
            } else {
                tvp->tv_usec = (shortest->when_ms - now_ms)*1000;
            }
            if (tvp->tv_sec < 0) tvp->tv_sec = 0;
            if (tvp->tv_usec < 0) tvp->tv_usec = 0;
        } else {//没有定时器，那么要么不等待立即返回，要么永远等待。
            /* If we have to check for events but need to return
             * ASAP because of AE_DONT_WAIT we need to set the timeout
             * to zero */
            if (flags & AE_DONT_WAIT) {
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            } else {
                /* Otherwise we can block */
                tvp = NULL; /* wait forever */
            }
        }
		//到这里后，tvp指向一个超时结构，代表epoll能够等待的时间。
		//调用epoll_wait，返回可读/写的事件数目，并且将其设置到fired数组上面，下面进行实际的事件处理。
        numevents = aeApiPoll(eventLoop, tvp);
        for (j = 0; j < numevents; j++) {
			//一个个扫描eventLoop->fired数组里面的，已经fired的连接句柄，
			//也就是循环处理每个连接的读写事件。aeApiPoll只是返回是否有事件。
            aeFileEvent *fe = &eventLoop->events[eventLoop->fired[j].fd];
            int mask = eventLoop->fired[j].mask;
            int fd = eventLoop->fired[j].fd;
            int rfired = 0;

	    /* note the fe->mask & mask & ... code: maybe an already processed
             * event removed an element that fired and we still didn't
             * processed, so we check if the event is still valid. */
            if (fe->mask & mask & AE_READABLE) {//处理读事件
                rfired = 1;
				//accept连接为acceptTcpHandler，initServer设置的。
				//设置的读句柄为readQueryFromClient，读取客户端的连接。
                fe->rfileProc(eventLoop,fd,fe->clientData,mask);
            }
            if (fe->mask & mask & AE_WRITABLE) {//处理写事件。
                if (!rfired || fe->wfileProc != fe->rfileProc)//如果没有处理可读事件 或者 读写句柄不相同，那么需要调用其函数。
                //给客户端发送数据的处理函数为sendReplyToClient
                    fe->wfileProc(eventLoop,fd,fe->clientData,mask);
            }
            processed++;
        }
    }
	
	//循环判断eventLoop->timeEventHead链表的事件是否到达时间，如果到了就调用其timeProc回调。
    /* Check time events */
    if (flags & AE_TIME_EVENTS)
        processed += processTimeEvents(eventLoop);

    return processed; /* return the number of processed file/time events */
}

/* Wait for milliseconds until the given file descriptor becomes
 * writable/readable/exception */
int aeWait(int fd, int mask, long long milliseconds) {
    struct pollfd pfd;
    int retmask = 0, retval;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    if (mask & AE_READABLE) pfd.events |= POLLIN;
    if (mask & AE_WRITABLE) pfd.events |= POLLOUT;

    if ((retval = poll(&pfd, 1, milliseconds))== 1) {
        if (pfd.revents & POLLIN) retmask |= AE_READABLE;
        if (pfd.revents & POLLOUT) retmask |= AE_WRITABLE;
	if (pfd.revents & POLLERR) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLHUP) retmask |= AE_WRITABLE;
        return retmask;
    } else {
        return retval;
    }
}

void aeMain(aeEventLoop *eventLoop) {
    eventLoop->stop = 0;
    while (!eventLoop->stop) {//stop ==1 停止服务
        if (eventLoop->beforesleep != NULL)
            eventLoop->beforesleep(eventLoop);//调用了beforeSleep函数。
		
        aeProcessEvents(eventLoop, AE_ALL_EVENTS);//处理各种事件。
    }
}

char *aeGetApiName(void) {
    return aeApiName();
}

void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep) {
    eventLoop->beforesleep = beforesleep;
}
