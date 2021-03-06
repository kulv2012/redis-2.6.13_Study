/* zmalloc - total amount of allocated memory aware version of malloc()
 *
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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
#include <stdlib.h>

/* This function provide us access to the original libc free(). This is useful
 * for instance to free results obtained by backtrace_symbols(). We need
 * to define this function before including zmalloc.h that may shadow the
 * free implementation if we use jemalloc or another non standard allocator. */
void zlibc_free(void *ptr) {
    free(ptr);
}

#include <string.h>
#include <pthread.h>
#include "config.h"
#include "zmalloc.h"

#ifdef HAVE_MALLOC_SIZE 
//是否有根据指针获取该块内存大小的库函数，如果没有，那在redis分配的内存块前面会包含4/8个字节的头部用来记录本块内存的大小。否则直接调用函数获取。
#define PREFIX_SIZE (0)
#else
#if defined(__sun) || defined(__sparc) || defined(__sparc__)
#define PREFIX_SIZE (sizeof(long long))
#else
#define PREFIX_SIZE (sizeof(size_t))
#endif
#endif

/* Explicitly override malloc/free etc when using tcmalloc. */
#if defined(USE_TCMALLOC)
#define malloc(size) tc_malloc(size)
#define calloc(count,size) tc_calloc(count,size)
#define realloc(ptr,size) tc_realloc(ptr,size)
#define free(ptr) tc_free(ptr)
#elif defined(USE_JEMALLOC)
#define malloc(size) je_malloc(size)
#define calloc(count,size) je_calloc(count,size)
#define realloc(ptr,size) je_realloc(ptr,size)
#define free(ptr) je_free(ptr)
#endif

#ifdef HAVE_ATOMIC
#define update_zmalloc_stat_add(__n) __sync_add_and_fetch(&used_memory, (__n))
#define update_zmalloc_stat_sub(__n) __sync_sub_and_fetch(&used_memory, (__n))
#else
#define update_zmalloc_stat_add(__n) do { \
    pthread_mutex_lock(&used_memory_mutex); \
    used_memory += (__n); \
    pthread_mutex_unlock(&used_memory_mutex); \
} while(0)

#define update_zmalloc_stat_sub(__n) do { \
    pthread_mutex_lock(&used_memory_mutex); \
    used_memory -= (__n); \
    pthread_mutex_unlock(&used_memory_mutex); \
} while(0)

#endif

//下面将新分配的内存大小计数到used_memory全局变量中，如果zmalloc_thread_safe为1的话枷锁，然后增加到used_memory
#define update_zmalloc_stat_alloc(__n) do { \
    size_t _n = (__n); \
    if (_n&(sizeof(long)-1)) _n += sizeof(long)-(_n&(sizeof(long)-1)); \
    if (zmalloc_thread_safe) { \
        update_zmalloc_stat_add(_n); \
    } else { \
        used_memory += _n; \
    } \
} while(0)

#define update_zmalloc_stat_free(__n) do { \
    size_t _n = (__n); \
    if (_n&(sizeof(long)-1)) _n += sizeof(long)-(_n&(sizeof(long)-1)); \
    if (zmalloc_thread_safe) { \
        update_zmalloc_stat_sub(_n); \
    } else { \
        used_memory -= _n; \
    } \
} while(0)

static size_t used_memory = 0;
static int zmalloc_thread_safe = 0;//奇怪，这份代码 中，通篇都只有1个地方修改为1，在main开始，
//那设置这个有用吗? used_memory在单线程下不需要保护，肯定为多线程。莫非是由于遗留问题，这个留在这里了?
pthread_mutex_t used_memory_mutex = PTHREAD_MUTEX_INITIALIZER;

static void zmalloc_default_oom(size_t size) {
    fprintf(stderr, "zmalloc: Out of memory trying to allocate %zu bytes\n", size);
    fflush(stderr);
    abort();
}

static void (*zmalloc_oom_handler)(size_t) = zmalloc_default_oom;

void *zmalloc(size_t size) {
    void *ptr = malloc(size+PREFIX_SIZE);

    if (!ptr) zmalloc_oom_handler(size);//如果内存申请失败，就挂掉，停止程序。呃····有点猛
#ifdef HAVE_MALLOC_SIZE //如果有malloc_size函数，如使用了google的tcmalloc库或者在MAC下，则只要根据malloc返回的指针，获取这块内存的大小
    update_zmalloc_stat_alloc(zmalloc_size(ptr));//然后将新申请的大小向上取整后增加到used_memory变量中
    return ptr;//返回malloc的大小。
#else
    *((size_t*)ptr) = size;//否则的话，使用之前分配的PREFIX_SIZE字节，记录用户申请的内存大小到内存块的前面。
    update_zmalloc_stat_alloc(size+PREFIX_SIZE);//然后统计大小。
    return (char*)ptr+PREFIX_SIZE;//返回后面的数据给上层应用。那么，这个大小记录这到底是用来干什么的呢
#endif
}
//跟上面的zmalloc基本相同
void *zcalloc(size_t size) {//man说: The memory is set to zero，malloc不会进行初始化，仅此而已。
    void *ptr = calloc(1, size+PREFIX_SIZE);

    if (!ptr) zmalloc_oom_handler(size);
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_alloc(zmalloc_size(ptr));
    return ptr;
#else
    *((size_t*)ptr) = size;
    update_zmalloc_stat_alloc(size+PREFIX_SIZE);
    return (char*)ptr+PREFIX_SIZE;
#endif
}

void *zrealloc(void *ptr, size_t size) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
#endif
    size_t oldsize;
    void *newptr;

    if (ptr == NULL) return zmalloc(size);
#ifdef HAVE_MALLOC_SIZE //如果有malloc_size，则获取其大小，然后relloac，然后更新分配的大小数据，也就是used_memory
    oldsize = zmalloc_size(ptr);
    newptr = realloc(ptr,size);
    if (!newptr) zmalloc_oom_handler(size);

    update_zmalloc_stat_free(oldsize);
    update_zmalloc_stat_alloc(zmalloc_size(newptr));
    return newptr;
#else
    realptr = (char*)ptr-PREFIX_SIZE;//得到前面记录大小的字节，下面需要修改大小的。
    oldsize = *((size_t*)realptr);
    newptr = realloc(realptr,size+PREFIX_SIZE);
    if (!newptr) zmalloc_oom_handler(size);

    *((size_t*)newptr) = size;
    update_zmalloc_stat_free(oldsize);
    update_zmalloc_stat_alloc(size);
    return (char*)newptr+PREFIX_SIZE;
#endif
}

/* Provide zmalloc_size() for systems where this function is not provided by
 * malloc itself, given that in that case we store an header with this
 * information as the first bytes of every allocation. */
#ifndef HAVE_MALLOC_SIZE
size_t zmalloc_size(void *ptr) {//直接看上面的解释吧，通过前面的字节获取内存大小。返回。
    void *realptr = (char*)ptr-PREFIX_SIZE;
    size_t size = *((size_t*)realptr);
    /* Assume at least that all the allocations are padded at sizeof(long) by
     * the underlying allocator. */
    if (size&(sizeof(long)-1)) size += sizeof(long)-(size&(sizeof(long)-1));
    return size+PREFIX_SIZE;
}
#endif

void zfree(void *ptr) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
    size_t oldsize;
#endif

    if (ptr == NULL) return;
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_free(zmalloc_size(ptr));//减少内存计数，然后释放空间。
    free(ptr);
#else
    realptr = (char*)ptr-PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    update_zmalloc_stat_free(oldsize+PREFIX_SIZE);
    free(realptr);
#endif
}

char *zstrdup(const char *s) {
    size_t l = strlen(s)+1;//这样用，那以后是不是得经常strlen了?不怕性能问题么
    char *p = zmalloc(l);

    memcpy(p,s,l);
    return p;
}

size_t zmalloc_used_memory(void) {//就获取了used_memory变量，啥也没干。
    size_t um;
    if (zmalloc_thread_safe) {
#ifdef HAVE_ATOMIC
        um = __sync_add_and_fetch(&used_memory, 0);
#else
        pthread_mutex_lock(&used_memory_mutex);
        um = used_memory;
        pthread_mutex_unlock(&used_memory_mutex);
#endif
    }
    else {
        um = used_memory;
    }
    return um;
}

void zmalloc_enable_thread_safeness(void) {
    zmalloc_thread_safe = 1;//这个我纳闷，这个变量整个系统中就没见它变过。
}

//设置内存分配malloc失败后调用的处理回调，main最开头会调用设置为redisOutOfMemoryHandler，这个函数基本打印几行日志后，就让自己core了。
void zmalloc_set_oom_handler(void (*oom_handler)(size_t)) {
    zmalloc_oom_handler = oom_handler;
}

/* Get the RSS information in an OS-specific way.
 *
 * WARNING: the function zmalloc_get_rss() is not designed to be fast
 * and may not be called in the busy loops where Redis tries to release
 * memory expiring or swapping out objects.
 *
 * For this kind of "fast RSS reporting" usages use instead the
 * function RedisEstimateRSS() that is a much faster (and less precise)
 * version of the function. */

#if defined(HAVE_PROCFS)
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

size_t zmalloc_get_rss(void) {//获取进程驻留在物理内存中的内存数目。
    int page = sysconf(_SC_PAGESIZE);
    size_t rss;
    char buf[4096];
    char filename[256];
    int fd, count;
    char *p, *x;
//RSS: "Resident Set Size", 实际驻留"在物理内存中"的内存数. 不包括已经交换出去的代码. 
//举一个例子: 如果你有一个程序使用了100K内存, 操作系统交换出40K内存, 那么RSS为60K. 
//RSS还包括了与其它进程共享的内存区域. 这些区域通常用于libc库等.共享的部分从SHARE可以获取到。
//The resident set size is the portion of a process's memory that is held in RAM. 
//The rest of the memory exists in swap or the filesystem (never loaded or previously unloaded parts of the executable).
    snprintf(filename,256,"/proc/%d/stat",getpid());
    if ((fd = open(filename,O_RDONLY)) == -1) return 0;
    if (read(fd,buf,4096) <= 0) {
        close(fd);
        return 0;
    }
    close(fd);

    p = buf;
    count = 23; /* RSS is the 24th field in /proc/<pid>/stat */
    while(p && count--) {
        p = strchr(p,' ');
        if (p) p++;
    }
    if (!p) return 0;
    x = strchr(p,' ');
    if (!x) return 0;
    *x = '\0';
    rss = strtoll(p,NULL,10);
    rss *= page;
    return rss;
}
#elif defined(HAVE_TASKINFO)
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/task.h>
#include <mach/mach_init.h>

size_t zmalloc_get_rss(void) {
    task_t task = MACH_PORT_NULL;
    struct task_basic_info t_info;
    mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;

    if (task_for_pid(current_task(), getpid(), &task) != KERN_SUCCESS)
        return 0;
    task_info(task, TASK_BASIC_INFO, (task_info_t)&t_info, &t_info_count);

    return t_info.resident_size;
}
#else
size_t zmalloc_get_rss(void) {//否则如果如法从操作系统的层面获取驻留内存大小，那就只能绌劣的返回已经分配出去的内存大小
    /* If we can't get the RSS in an OS-specific way for this system just
     * return the memory usage we estimated in zmalloc()..
     *
     * Fragmentation will appear to be always 1 (no fragmentation)
     * of course... */
    return zmalloc_used_memory();
}
#endif

/* Fragmentation = RSS / allocated-bytes */
float zmalloc_get_fragmentation_ratio(void) {//提供给genRedisInfoString返回内存使用信息。
//直接用驻留在物理内存中的内存/除以 分配的总物理内存，得到一个所谓的碎片率， 实际留在物理内存中的除以总分配的
//如果程序曾经申请了30G的内存，后来free了29G，它的used_memory为1G，但是rss很可能会很大，这样碎片率就很高。
//当然如果存在swap就不一定特别精准了，比如那29G都swap到磁盘了
    return (float)zmalloc_get_rss()/zmalloc_used_memory();
}

#if defined(HAVE_PROCFS)
size_t zmalloc_get_private_dirty(void) {//得到本进程的脏虚拟页面大小。
    char line[1024];
    size_t pd = 0;
    FILE *fp = fopen("/proc/self/smaps","r");//该文件是调用进程的进程内存映像信息，比同一目录下的maps文件更详细。

    if (!fp) return 0;
    while(fgets(line,sizeof(line),fp) != NULL) {
        if (strncmp(line,"Private_Dirty:",14) == 0) {//如果是Private_Dirty 已改写的私有页面，则将他们全部加起来。
            char *p = strchr(line,'k');
            if (p) {
                *p = '\0';
                pd += strtol(line+14,NULL,10) * 1024;
            }
        }
    }
    fclose(fp);
    return pd;
}
#else
size_t zmalloc_get_private_dirty(void) {
    return 0;
}
#endif
