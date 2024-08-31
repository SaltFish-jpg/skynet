#include "skynet.h"
#include "skynet_server.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_module.h"
#include "skynet_timer.h"
#include "skynet_monitor.h"
#include "skynet_socket.h"
#include "skynet_daemon.h"
#include "skynet_harbor.h"

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

// 监视器
struct monitor {
    int count; // 工作线程总数
    struct skynet_monitor **m; // 每个工作线程的监视器
    pthread_cond_t cond; // 互斥条件
    pthread_mutex_t mutex; // 互斥量
    int sleep; // 睡眠中工作线程数量
    int quit; // 线程退出标记
};

struct worker_parm {
    struct monitor *m; // 总监视器,共用的
    int id; // 工作线程id
    int weight; // 线程权重
};

static volatile int SIG = 0;

/**
 * SIGHUP 信号在用户终端连接(正常或非正常)结束时发出, 通常是在终端的控制进程结束时, 通知同一session内的各个作业,
 * 这时它们与控制终端不再关联. 系统对SIGHUP信号的默认处理是终止收到该信号的进程。所以若程序中没有捕捉该信号，当收到该信号时，进程就会退出。
 *
 * @param signal
 */
static void
handle_hup(int signal) {
    if (signal == SIGHUP) {
        SIG = 1;
    }
}

#define CHECK_ABORT if (skynet_context_total()==0) break;

/**
 * 创建工作线程
 * @param thread
 * @param start_routine
 * @param arg
 */
static void
create_thread(pthread_t *thread, void *(*start_routine)(void *), void *arg) {
    // pthread_create () 函数在调用进程中启动一个新线程。新线程通过调用start_routine()开始执行,arg作为start_routine()的唯一参数传递
    if (pthread_create(thread, NULL, start_routine, arg)) {
        fprintf(stderr, "Create thread failed");
        exit(1);
    }
}

/**
 * 唤醒工作线程
 * @param m
 * @param busy 忙碌线程个数
 */
static void
wakeup(struct monitor *m, int busy) {
    if (m->sleep >= m->count - busy) {
        // signal sleep worker, "spurious wakeup" is harmless
        // 向睡眠工作者发出信号，“虚假唤醒”是无害的
        pthread_cond_signal(&m->cond);
    }
}

static void *
 thread_socket(void *p) {
    struct monitor *m = p;
    // 线程标记——网络线程
    skynet_initthread(THREAD_SOCKET);
    for (;;) {
        // 开始做处理
        int r = skynet_socket_poll();
        // 网络线程退出
        if (r == 0)
            break;
        // 还有消息 -1
        if (r < 0) {
            // 检查中止 if (skynet_context_total()==0) break;
            CHECK_ABORT
            continue;
        }
        // return 1
        // 唤醒工作线程去处理
        wakeup(m, 0);
    }
    return NULL;
}

static void
free_monitor(struct monitor *m) {
    int i;
    int n = m->count;
    for (i = 0; i < n; i++) {
        skynet_monitor_delete(m->m[i]);
    }
    pthread_mutex_destroy(&m->mutex);
    pthread_cond_destroy(&m->cond);
    skynet_free(m->m);
    skynet_free(m);
}

static void *
thread_monitor(void *p) {
    struct monitor *m = p;
    int i;
    int n = m->count;
    // 线程标记
    skynet_initthread(THREAD_MONITOR);
    for (;;) {
        CHECK_ABORT
        for (i = 0; i < n; i++) {
            // 检查工作线程是否出现循环
            skynet_monitor_check(m->m[i]);
        }
        for (i = 0; i < 5; i++) {
            CHECK_ABORT
            sleep(1);
        }
    }
    return NULL;
}

static void
signal_hup() {
    // make log file reopen

    struct skynet_message smsg;
    smsg.source = 0;
    smsg.session = 0;
    smsg.data = NULL;
    smsg.sz = (size_t) PTYPE_SYSTEM << MESSAGE_TYPE_SHIFT;
    uint32_t logger = skynet_handle_findname("logger");
    if (logger) {
        skynet_context_push(logger, &smsg);
    }
}

static void *
thread_timer(void *p) {
    struct monitor *m = p;
    // 定时线程
    skynet_initthread(THREAD_TIMER);
    for (;;) {
        skynet_updatetime();
        skynet_socket_updatetime();
        CHECK_ABORT
        wakeup(m, m->count - 1);
        usleep(2500);
        if (SIG) {
            signal_hup();
            SIG = 0;
        }
    }
    // wakeup socket thread
    skynet_socket_exit();
    // wakeup all worker thread
    pthread_mutex_lock(&m->mutex);
    m->quit = 1;
    pthread_cond_broadcast(&m->cond);
    pthread_mutex_unlock(&m->mutex);
    return NULL;
}

/**
 * 工作线程的loop
 * @param p
 * @return
 */
static void *
thread_worker(void *p) {
    // 工作线程参数
    struct worker_parm *wp = p;
    int id = wp->id;
    int weight = wp->weight;
    struct monitor *m = wp->m;
    struct skynet_monitor *sm = m->m[id];
    // 本地线程变量,标记工作线程
    skynet_initthread(THREAD_WORKER);
    struct message_queue *q = NULL;
    while (!m->quit) {
        // 全局队列为空,没有任务可执行
        q = skynet_context_message_dispatch(sm, q, weight);
        if (q == NULL) {
            if (pthread_mutex_lock(&m->mutex) == 0) {
                ++m->sleep;
                // "spurious wakeup" is harmless,
                // because skynet_context_message_dispatch() can be call at any time.
                if (!m->quit)
                    pthread_cond_wait(&m->cond, &m->mutex);
                --m->sleep;
                if (pthread_mutex_unlock(&m->mutex)) {
                    fprintf(stderr, "unlock mutex error");
                    exit(1);
                }
            }
        }
    }
    return NULL;
}

static void
start(int thread) {
    // 线程id数组
    pthread_t pid[thread + 3];
    // malloc()函数保留指定字节数的内存块。并且，它返回一个可以转换为任何形式指针的指针
    struct monitor *m = skynet_malloc(sizeof(*m));
    memset(m, 0, sizeof(*m));
    // 工作线程总数量
    m->count = thread;
    // 睡眠中工作线程数量
    m->sleep = 0;
    // skynet监视器
    m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *));
    int i;
    // 初始化工作线程监视器
    for (i = 0; i < thread; i++) {
        m->m[i] = skynet_monitor_new();
    }
    // 线程互斥量
    if (pthread_mutex_init(&m->mutex, NULL)) {
        fprintf(stderr, "Init mutex error");
        exit(1);
    }
    // 条件变量
    if (pthread_cond_init(&m->cond, NULL)) {
        fprintf(stderr, "Init cond error");
        exit(1);
    }

    create_thread(&pid[0], thread_monitor, m);
    create_thread(&pid[1], thread_timer, m);
    create_thread(&pid[2], thread_socket, m);
    // 工作线程权重 32个
    static int weight[] = {
            -1, -1, -1, -1, 0, 0, 0, 0,
            1, 1, 1, 1, 1, 1, 1, 1,
            2, 2, 2, 2, 2, 2, 2, 2,
            3, 3, 3, 3, 3, 3, 3, 3,};
    struct worker_parm wp[thread];
    for (i = 0; i < thread; i++) {
        // 监视器
        wp[i].m = m;
        // 工作线程id
        wp[i].id = i;
        // 前32个按照权重数组来
        if (i < sizeof(weight) / sizeof(weight[0])) {
            wp[i].weight = weight[i];
        } else {
            wp[i].weight = 0;
        }
        create_thread(&pid[i + 3], thread_worker, &wp[i]);
    }

    for (i = 0; i < thread + 3; i++) {
        // 主线程join其它线程
        pthread_join(pid[i], NULL);
    }
    // 到这就是退出了,释放监视器
    free_monitor(m);
}

static void
bootstrap(struct skynet_context *logger, const char *cmdline) {
    int sz = strlen(cmdline);
    char name[sz + 1];
    char args[sz + 1];
    int arg_pos;
    sscanf(cmdline, "%s", name);
    arg_pos = strlen(name);
    if (arg_pos < sz) {
        while (cmdline[arg_pos] == ' ') {
            arg_pos++;
        }
        strncpy(args, cmdline + arg_pos, sz);
    } else {
        args[0] = '\0';
    }
    struct skynet_context *ctx = skynet_context_new(name, args);
    if (ctx == NULL) {
        skynet_error(NULL, "Bootstrap error : %s\n", cmdline);
        skynet_context_dispatchall(logger);
        exit(1);
    }
}

void
skynet_start(struct skynet_config *config) {
    // register SIGHUP for log file reopen
    // 注册 SIGHUP 以重新打开日志文件
    //  struct sigaction {
    //               void     (*sa_handler)(int);
    //               void     (*sa_sigaction)(int, siginfo_t *, void *);
    //               sigset_t   sa_mask;
    //               int        sa_flags;
    //               void     (*sa_restorer)(void);
    //           };
    // 在某些体系结构上涉及联合：不要同时分配给sa_handler和sa_sigaction。sa_restorer字段不适用于应用程序使用。
    struct sigaction sa;
    sa.sa_handler = &handle_hup;
    // SA_RESTART 通过使某些系统调用可跨信号重新启动，提供与 BSD 信号语义兼容的行为。此标志仅在建立信号处理程序时才有意义。
    sa.sa_flags = SA_RESTART;
    sigfillset(&sa.sa_mask);
    // int sigaction (int signum,const struct sigaction *_Nullable restrict act,struct sigaction *_Nullable restrict oldact)
    // sigaction()系统调用,用于改变进程收到特定信号时的处理。
    // signum指定信号，可以是除SIGKILL和SIGSTOP之外的任何有效信号。
    // 如果act非空，则安装信号signum的新操作来自act。如果oldact非 NULL，则前一个操作将保存在oldact中。
    sigaction(SIGHUP, &sa, NULL);
    // 守护进程
    if (config->daemon) {
        if (daemon_init(config->daemon)) {
            exit(1);
        }
    }
    skynet_harbor_init(config->harbor);
    skynet_handle_init(config->harbor);
    skynet_mq_init();
    skynet_module_init(config->module_path);
    skynet_timer_init();
    skynet_socket_init();
    skynet_profile_enable(config->profile);

    struct skynet_context *ctx = skynet_context_new(config->logservice, config->logger);
    if (ctx == NULL) {
        fprintf(stderr, "Can't launch %s service\n", config->logservice);
        exit(1);
    }

    skynet_handle_namehandle(skynet_context_handle(ctx), "logger");

    bootstrap(ctx, config->bootstrap);

    start(config->thread);

    // harbor_exit may call socket send, so it should exit before socket_free
    skynet_harbor_exit();
    skynet_socket_free();
    if (config->daemon) {
        daemon_exit(config->daemon);
    }
}
