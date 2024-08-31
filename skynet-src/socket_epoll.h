#ifndef poll_socket_epoll_h
#define poll_socket_epoll_h

#include <netdb.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

static bool
sp_invalid(int efd) {
    return efd == -1;
}

static int
sp_create() {
    //  在最初的epoll_create () 实现中，size参数通知内核文件描述符的数量调用者期望添加到epoll实例中。
    //  内核使用的此信息提示最初在描述事件的内部数据结构中分配。（如果必要时，如果调用者的使用量超出了大小给出的提示。）
    //  现在，这个提示不再需要（内核动态调整所需的数据结构，而不需要提示），但大小仍然必须是大于零，以确保向后兼容
    //  新的epoll应用程序在旧内核上运行
    return epoll_create(1024);
}

static void
sp_release(int efd) {
    close(efd);
}

static int
sp_add(int efd, int sock, void *ud) {
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = ud;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, sock, &ev) == -1) {
        return 1;
    }
    return 0;
}

/**
 * 删除socket监听
 * @param efd
 * @param sock
 */
static void
sp_del(int efd, int sock) {
    epoll_ctl(efd, EPOLL_CTL_DEL, sock, NULL);
}

/**
 * 开启监听
 *
 * @param efd
 * @param sock
 * @param ud
 * @param read_enable
 * @param write_enable
 * @return  成功返回0,失败返回1
 */
static int
sp_enable(int efd, int sock, void *ud, bool read_enable, bool write_enable) {
    struct epoll_event ev;
    ev.events = (read_enable ? EPOLLIN : 0) | (write_enable ? EPOLLOUT : 0);
    ev.data.ptr = ud;
    if (epoll_ctl(efd, EPOLL_CTL_MOD, sock, &ev) == -1) {
        return 1;
    }
    return 0;
}

static int
sp_wait(int efd, struct event *e, int max) {
    struct epoll_event ev[max];
    int n = epoll_wait(efd, ev, max, -1);
    int i;
    for (i = 0; i < n; i++) {
        e[i].s = ev[i].data.ptr;
        unsigned flag = ev[i].events;
        e[i].write = (flag & EPOLLOUT) != 0;
        e[i].read = (flag & EPOLLIN) != 0;
        e[i].error = (flag & EPOLLERR) != 0;
        e[i].eof = (flag & EPOLLHUP) != 0;
    }

    return n;
}

static void
sp_nonblocking(int fd) {
    // 当第二个参数cmd=F_GETFL时，它的作用是取得文件描述符filedes的文件状态标志。
    // 当第二个参数cmd=F_SETFL时，它的作用是设置文件描述符filedes的文件状态标志，这时第三个参数为新的状态标志。
    int flag = fcntl(fd, F_GETFL, 0);
    if (-1 == flag) {
        return;
    }

    fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

#endif
