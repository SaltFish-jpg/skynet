#ifndef poll_socket_kqueue_h
#define poll_socket_kqueue_h

#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static bool
sp_invalid(int kfd) {
    return kfd == -1;
}

static int
sp_create() {
    return kqueue();
}

static void
sp_release(int kfd) {
    close(kfd);
}

/**
 * 删除读写事件监听
 * @param kfd kqueue()的描述符
 * @param sock 目标socket
 */
static void
sp_del(int kfd, int sock) {
    struct kevent ke;
    EV_SET(&ke, sock, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(kfd, &ke, 1, NULL, 0, NULL);
    EV_SET(&ke, sock, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(kfd, &ke, 1, NULL, 0, NULL);
}

static int
sp_add(int kfd, int sock, void *ud) {
    struct kevent ke;
    EV_SET(&ke, sock, EVFILT_READ, EV_ADD, 0, 0, ud);
    if (kevent(kfd, &ke, 1, NULL, 0, NULL) == -1 || ke.flags & EV_ERROR) {
        // 注册读事件失败
        return 1;
    }
    EV_SET(&ke, sock, EVFILT_WRITE, EV_ADD, 0, 0, ud);
    if (kevent(kfd, &ke, 1, NULL, 0, NULL) == -1 || ke.flags & EV_ERROR) {
        // 注册写事件失败后,要删除掉之前注册成功的读事件
        EV_SET(&ke, sock, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        kevent(kfd, &ke, 1, NULL, 0, NULL);
        return 1;
    }
    // 这里暂时禁用了写事件监听,刚开始没有写数据失败,不需要写事件监听
    EV_SET(&ke, sock, EVFILT_WRITE, EV_DISABLE, 0, 0, ud);
    if (kevent(kfd, &ke, 1, NULL, 0, NULL) == -1 || ke.flags & EV_ERROR) {
        // 禁用监听失败后,删除之前的读写事件
        sp_del(kfd, sock);
        return 1;
    }
    return 0;
}

static int
sp_enable(int kfd, int sock, void *ud, bool read_enable, bool write_enable) {
    int ret = 0;
    struct kevent ke;
    EV_SET(&ke, sock, EVFILT_READ, read_enable ? EV_ENABLE : EV_DISABLE, 0, 0, ud);
    if (kevent(kfd, &ke, 1, NULL, 0, NULL) == -1 || ke.flags & EV_ERROR) {
        ret |= 1;
    }
    EV_SET(&ke, sock, EVFILT_WRITE, write_enable ? EV_ENABLE : EV_DISABLE, 0, 0, ud);
    if (kevent(kfd, &ke, 1, NULL, 0, NULL) == -1 || ke.flags & EV_ERROR) {
        ret |= 1;
    }
    return ret;
}

static int
sp_wait(int kfd, struct event *e, int max) {
    struct kevent ev[max];
    int n = kevent(kfd, NULL, 0, ev, max, NULL);

    int i;
    for (i = 0; i < n; i++) {
        e[i].s = ev[i].udata;
        unsigned filter = ev[i].filter;
        bool eof = (ev[i].flags & EV_EOF) != 0;
        e[i].write = (filter == EVFILT_WRITE) && (!eof);
        e[i].read = (filter == EVFILT_READ);
        e[i].error = (ev[i].flags & EV_ERROR) != 0;
        e[i].eof = eof;
    }

    return n;
}

static void
sp_nonblocking(int fd) {
    // 第二个参数cmd=F_GETFL,取得文件描述符fd的文件状态标志
    // 若成功则返回文件状态标志，若出错则返回-1
    int flag = fcntl(fd, F_GETFL, 0);
    if (-1 == flag) {
        return;
    }
    // 第二个参数cmd=F_SETFL,设置文件描述符fd的文件状态标志，这时第三个参数为新的状态标志
    fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

#endif
