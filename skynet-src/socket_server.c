#include "skynet.h"

#include "socket_server.h"
#include "socket_poll.h"
#include "atomic.h"
#include "spinlock.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#define MAX_INFO 128
// MAX_SOCKET will be 2^MAX_SOCKET_P
#define MAX_SOCKET_P 16
#define MAX_EVENT 64
#define MIN_READ_BUFFER 64
#define SOCKET_TYPE_INVALID 0
#define SOCKET_TYPE_RESERVE 1
#define SOCKET_TYPE_PLISTEN 2
#define SOCKET_TYPE_LISTEN 3
#define SOCKET_TYPE_CONNECTING 4
#define SOCKET_TYPE_CONNECTED 5
#define SOCKET_TYPE_HALFCLOSE_READ 6
#define SOCKET_TYPE_HALFCLOSE_WRITE 7
#define SOCKET_TYPE_PACCEPT 8
#define SOCKET_TYPE_BIND 9

#define MAX_SOCKET (1<<MAX_SOCKET_P)

#define PRIORITY_HIGH 0
#define PRIORITY_LOW 1

#define HASH_ID(id) (((unsigned)id) % MAX_SOCKET)
#define ID_TAG16(id) ((id>>MAX_SOCKET_P) & 0xffff)

#define PROTOCOL_TCP 0
#define PROTOCOL_UDP 1
#define PROTOCOL_UDPv6 2
#define PROTOCOL_UNKNOWN 255

#define UDP_ADDRESS_SIZE 19    // ipv6 128bit + port 16bit + 1 byte type

#define MAX_UDP_PACKAGE 65535

// EAGAIN and EWOULDBLOCK may be not the same value.
#if (EAGAIN != EWOULDBLOCK)
#define AGAIN_WOULDBLOCK EAGAIN : case EWOULDBLOCK
#else
#define AGAIN_WOULDBLOCK EAGAIN
#endif

#define WARNING_SIZE (1024*1024)

#define USEROBJECT ((size_t)(-1)) // 用来区分是否用户数据obj

struct write_buffer {
    struct write_buffer *next; // 链表指针
    const void *buffer; // 需要写入缓冲区的数据
    char *ptr; // 开始写入的偏移量
    size_t sz; // buffer剩余需要写入发送缓冲区大小
    bool userobject; // 是否是用户obj
};

struct write_buffer_udp {
    struct write_buffer buffer;
    uint8_t udp_address[UDP_ADDRESS_SIZE];
};

struct wb_list {
    struct write_buffer *head;
    struct write_buffer *tail;
};

struct socket_stat {
    uint64_t rtime;
    uint64_t wtime;
    uint64_t read;
    uint64_t write;
};

struct socket {
    // 不透明数据
    uintptr_t opaque;
    // 高低两个待发送缓冲区作用？
    // wait_buffer
    struct wb_list high; // 待发送数据链表
    struct wb_list low; // 待发送数据链表
    int64_t wb_size; // 等待写入的所有缓冲区数据量大小
    struct socket_stat stat; // 读写统计
    ATOM_ULONG sending; // 是否正在发送数据,引用计数累加
    int fd; // 对应的网络连接文件fd_id;
    int id; // socket id
    ATOM_INT type; // 连接状态
    uint8_t protocol; // 连接协议
    bool reading; // fd的read监听标记
    bool writing; // fd的write监听标记
    bool closing; // fd的close标记
    ATOM_INT udpconnecting; // udp 正在连接
    int64_t warn_size; // 待发送缓冲区警戒线大小
    union {
        int size;
        uint8_t udp_address[UDP_ADDRESS_SIZE]; // udp_addr
    } p;
    struct spinlock dw_lock; //direct write 直接写入锁
    int dw_offset; // 直接写入偏移量
    const void *dw_buffer; // 直接写入缓冲区
    size_t dw_size; // 直接写入缓冲区size
};

struct socket_server {
    volatile uint64_t time;
    int reserve_fd;   // for EMFILE  储备fd 当进程用尽文件描述符时，accept() 将失败并设置errno为 EMFILE
    int recvctrl_fd;  // 接收指令的fd
    int sendctrl_fd;  // 发送指令的fd
    int checkctrl;    // 检查指令的标志
    poll_fd event_fd; // epoll_fd
    ATOM_INT alloc_id; // 分配id
    int event_n;    // 当前事件个数
    int event_index; // 处理到的事件下标
    struct socket_object_interface soi; // 用户socket obj 相关接口
    struct event ev[MAX_EVENT]; // 事件数组
    struct socket slot[MAX_SOCKET]; // socket数组
    char buffer[MAX_INFO]; //
    uint8_t udpbuffer[MAX_UDP_PACKAGE];
    fd_set rfds; // fd集合
};

struct request_open {
    int id;
    int port;
    uintptr_t opaque;
    char host[1];
};

struct request_send {
    int id;
    size_t sz;
    const void *buffer;
};

struct request_send_udp {
    struct request_send send;
    uint8_t address[UDP_ADDRESS_SIZE];
};

struct request_setudp {
    int id;
    uint8_t address[UDP_ADDRESS_SIZE];
};

struct request_close {
    int id;
    int shutdown;
    uintptr_t opaque;
};

struct request_listen {
    int id;
    int fd;
    uintptr_t opaque;
    char host[1];
};

struct request_bind {
    int id;
    int fd;
    uintptr_t opaque;
};

struct request_resumepause {
    int id;
    uintptr_t opaque;
};

struct request_setopt {
    int id;
    int what;
    int value;
};

struct request_udp {
    int id;
    int fd;
    int family;
    uintptr_t opaque;
};

/*
	The first byte is TYPE

	S Start socket
	B Bind socket
	L Listen socket
	K Close socket
	O Connect to (Open)
	X Exit
	D Send package (high)
	P Send package (low)
	A Send UDP package
	T Set opt
	U Create UDP socket
	C set udp address
	Q query info
 */

struct request_package {
    uint8_t header[8];    // 6 bytes dummy
    union {
        char buffer[256];
        struct request_open open;
        struct request_send send;
        struct request_send_udp send_udp;
        struct request_close close;
        struct request_listen listen;
        struct request_bind bind;
        struct request_resumepause resumepause;
        struct request_setopt setopt;
        struct request_udp udp;
        struct request_setudp set_udp;
    } u;
    uint8_t dummy[256];
};

union sockaddr_all {
    struct sockaddr s;
    struct sockaddr_in v4;
    struct sockaddr_in6 v6;
};

struct send_object {
    const void *buffer; // 发送数据的buffer指针
    size_t sz; // 大小

    void (*free_func)(void *); // buffer释放方法
};

#define MALLOC skynet_malloc
#define FREE skynet_free

struct socket_lock {
    struct spinlock *lock;
    int count;
};

static inline void
socket_lock_init(struct socket *s, struct socket_lock *sl) {
    sl->lock = &s->dw_lock;
    sl->count = 0;
}

static inline void
socket_lock(struct socket_lock *sl) {
    if (sl->count == 0) {
        spinlock_lock(sl->lock);
    }
    ++sl->count;
}

// 可重入锁,当前线程尝试加锁成功后,后续都可以直接用
static inline int
socket_trylock(struct socket_lock *sl) {
    // count为0,线程没加锁成功
    if (sl->count == 0) {
        // count 为0,表示尝试加锁失败,直接返回
        if (!spinlock_trylock(sl->lock))
            return 0;    // lock failed
    }
    // 不为0(已经加锁成功) or 第一次加锁成功,增加计数
    ++sl->count;
    return 1;
}

static inline void
socket_unlock(struct socket_lock *sl) {
    --sl->count;
    if (sl->count <= 0) {
        assert(sl->count == 0);
        spinlock_unlock(sl->lock);
    }
}

static inline int
socket_invalid(struct socket *s, int id) {
    return (s->id != id || ATOM_LOAD(&s->type) == SOCKET_TYPE_INVALID);
}

/**
 * 初始化发送obj
 * @param ss
 * @param so
 * @param object
 * @param sz
 * @return true 是用户obj; false 不是
 */
static inline bool
send_object_init(struct socket_server *ss, struct send_object *so, const void *object, size_t sz) {
    // -1 为用户数据
    if (sz == USEROBJECT) {
        // 通用直接写入缓冲区的soi构造一个so,可以自定义下面三个接口
        so->buffer = ss->soi.buffer(object);
        so->sz = ss->soi.size(object);
        so->free_func = ss->soi.free;
        return true;
    } else {
        // 默认操作,用传进来的数据直接赋值
        so->buffer = object;
        so->sz = sz;
        so->free_func = FREE;
        return false;
    }
}

static void
dummy_free(void *ptr) {
    (void) ptr;
}

static inline void
send_object_init_from_sendbuffer(struct socket_server *ss, struct send_object *so, struct socket_sendbuffer *buf) {
    switch (buf->type) {
        case SOCKET_BUFFER_MEMORY:
            send_object_init(ss, so, buf->buffer, buf->sz);
            break;
        case SOCKET_BUFFER_OBJECT:
            send_object_init(ss, so, buf->buffer, USEROBJECT);
            break;
        case SOCKET_BUFFER_RAWPOINTER:
            so->buffer = buf->buffer;
            so->sz = buf->sz;
            so->free_func = dummy_free;
            break;
        default:
            // never get here
            so->buffer = NULL;
            so->sz = 0;
            so->free_func = NULL;
            break;
    }
}

static inline void
write_buffer_free(struct socket_server *ss, struct write_buffer *wb) {
    // 用户的数据,用户接口自己去释放
    if (wb->userobject) {
        ss->soi.free((void *) wb->buffer);
    } else {
        FREE((void *) wb->buffer);
    }
    FREE(wb);
}

static void
socket_keepalive(int fd) {
    int keepalive = 1;
    // 套接字保活
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *) &keepalive, sizeof(keepalive));
}

static int
reserve_id(struct socket_server *ss) {
    int i;
    for (i = 0; i < MAX_SOCKET; i++) {
        int id = ATOM_FINC(&(ss->alloc_id)) + 1;
        if (id < 0) {
            id = ATOM_FAND(&(ss->alloc_id), 0x7fffffff) & 0x7fffffff;
        }
        struct socket *s = &ss->slot[HASH_ID(id)];
        int type_invalid = ATOM_LOAD(&s->type);
        if (type_invalid == SOCKET_TYPE_INVALID) {
            if (ATOM_CAS(&s->type, type_invalid, SOCKET_TYPE_RESERVE)) {
                s->id = id;
                s->protocol = PROTOCOL_UNKNOWN;
                // socket_server_udp_connect may inc s->udpconncting directly (from other thread, before new_fd),
                // so reset it to 0 here rather than in new_fd.
                ATOM_INIT(&s->udpconnecting, 0);
                s->fd = -1;
                return id;
            } else {
                // retry
                --i;
            }
        }
    }
    return -1;
}

static inline void
clear_wb_list(struct wb_list *list) {
    list->head = NULL;
    list->tail = NULL;
}

struct socket_server *
socket_server_create(uint64_t time) {
    int i;
    int fd[2];
    poll_fd efd = sp_create();
    if (sp_invalid(efd)) {
        skynet_error(NULL, "socket-server: create event pool failed.");
        return NULL;
    }
    if (pipe(fd)) {
        sp_release(efd);
        skynet_error(NULL, "socket-server: create socket pair failed.");
        return NULL;
    }
    if (sp_add(efd, fd[0], NULL)) {
        // add recvctrl_fd to event poll
        skynet_error(NULL, "socket-server: can't add server fd to event pool.");
        close(fd[0]);
        close(fd[1]);
        sp_release(efd);
        return NULL;
    }

    struct socket_server *ss = MALLOC(sizeof(*ss));
    ss->time = time;
    ss->event_fd = efd;
    ss->recvctrl_fd = fd[0];
    ss->sendctrl_fd = fd[1];
    ss->checkctrl = 1;
    ss->reserve_fd = dup(1);    // reserve an extra fd for EMFILE

    for (i = 0; i < MAX_SOCKET; i++) {
        struct socket *s = &ss->slot[i];
        ATOM_INIT(&s->type, SOCKET_TYPE_INVALID);
        clear_wb_list(&s->high);
        clear_wb_list(&s->low);
        spinlock_init(&s->dw_lock);
    }
    ATOM_INIT(&ss->alloc_id, 0);
    ss->event_n = 0;
    ss->event_index = 0;
    memset(&ss->soi, 0, sizeof(ss->soi));
    FD_ZERO(&ss->rfds);
    assert(ss->recvctrl_fd < FD_SETSIZE);

    return ss;
}

void
socket_server_updatetime(struct socket_server *ss, uint64_t time) {
    ss->time = time;
}

static void
free_wb_list(struct socket_server *ss, struct wb_list *list) {
    struct write_buffer *wb = list->head;
    while (wb) {
        struct write_buffer *tmp = wb;
        wb = wb->next;
        write_buffer_free(ss, tmp);
    }
    list->head = NULL;
    list->tail = NULL;
}

static void
free_buffer(struct socket_server *ss, struct socket_sendbuffer *buf) {
    void *buffer = (void *) buf->buffer;
    switch (buf->type) {
        case SOCKET_BUFFER_MEMORY:
            FREE(buffer);
            break;
        case SOCKET_BUFFER_OBJECT:
            ss->soi.free(buffer);
            break;
        case SOCKET_BUFFER_RAWPOINTER:
            break;
    }
}

static const void *
clone_buffer(struct socket_sendbuffer *buf, size_t *sz) {
    switch (buf->type) {
        case SOCKET_BUFFER_MEMORY:
            *sz = buf->sz;
            return buf->buffer;
        case SOCKET_BUFFER_OBJECT:
            *sz = USEROBJECT;
            return buf->buffer;
        case SOCKET_BUFFER_RAWPOINTER:
            // It's a raw pointer, we need make a copy
            *sz = buf->sz;
            void *tmp = MALLOC(*sz);
            memcpy(tmp, buf->buffer, *sz);
            return tmp;
    }
    // never get here
    *sz = 0;
    return NULL;
}

static void
force_close(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
    result->id = s->id;
    result->ud = 0;
    result->data = NULL;
    result->opaque = s->opaque;
    uint8_t type = ATOM_LOAD(&s->type);
    // 无效套接字
    if (type == SOCKET_TYPE_INVALID) {
        return;
    }
    assert(type != SOCKET_TYPE_RESERVE);
    // 释放待发送列表
    free_wb_list(ss, &s->high);
    free_wb_list(ss, &s->low);
    // 删除读写事件的监听
    sp_del(ss->event_fd, s->fd);
    // 加锁
    socket_lock(l);
    // socket 不是绑定状态才关闭
    if (type != SOCKET_TYPE_BIND) {
        if (close(s->fd) < 0) {
            perror("close socket:");
        }
    }
    // 设置socket无效
    ATOM_STORE(&s->type, SOCKET_TYPE_INVALID);
    // 释放当前存在的直接写入缓冲区
    if (s->dw_buffer) {
        struct socket_sendbuffer tmp;
        tmp.buffer = s->dw_buffer;
        tmp.sz = s->dw_size;
        tmp.id = s->id;
        // 区分是否用户obj
        tmp.type = (tmp.sz == USEROBJECT) ? SOCKET_BUFFER_OBJECT : SOCKET_BUFFER_MEMORY;
        free_buffer(ss, &tmp);
        s->dw_buffer = NULL;
    }
    socket_unlock(l);
}

void
socket_server_release(struct socket_server *ss) {
    int i;
    struct socket_message dummy;
    for (i = 0; i < MAX_SOCKET; i++) {
        struct socket *s = &ss->slot[i];
        struct socket_lock l;
        socket_lock_init(s, &l);
        if (ATOM_LOAD(&s->type) != SOCKET_TYPE_RESERVE) {
            force_close(ss, s, &l, &dummy);
        }
        spinlock_destroy(&s->dw_lock);
    }
    close(ss->sendctrl_fd);
    close(ss->recvctrl_fd);
    sp_release(ss->event_fd);
    if (ss->reserve_fd >= 0)
        close(ss->reserve_fd);
    FREE(ss);
}

static inline void
check_wb_list(struct wb_list *s) {
    assert(s->head == NULL);
    assert(s->tail == NULL);
}

/**
 * 改变写监听
 * @param ss
 * @param s
 * @param enable 是否开启写监听
 * @return 成功返回0,失败返回1
 */
static inline int
enable_write(struct socket_server *ss, struct socket *s, bool enable) {
    if (s->writing != enable) {
        s->writing = enable;
        return sp_enable(ss->event_fd, s->fd, s, s->reading, enable);
    }
    return 0;
}

/**
 * 改变读监听
 * @param ss
 * @param s
 * @param enable 是否开启读监听
 * @return 成功返回0,失败返回1
 */
static inline int
enable_read(struct socket_server *ss, struct socket *s, bool enable) {
    if (s->reading != enable) {
        s->reading = enable;
        // 这里开启失败会返回1,开启成功返回0
        return sp_enable(ss->event_fd, s->fd, s, enable, s->writing);
    }
    // 正常返回是0
    return 0;
}

static struct socket *
new_fd(struct socket_server *ss, int id, int fd, int protocol, uintptr_t opaque, bool reading) {
    struct socket *s = &ss->slot[HASH_ID(id)];
    // 预定
    assert(ATOM_LOAD(&s->type) == SOCKET_TYPE_RESERVE);
    if (sp_add(ss->event_fd, fd, s)) {
        // 增加读写事件失败会返回1
        // 无效化
        ATOM_STORE(&s->type, SOCKET_TYPE_INVALID);
        return NULL;
    }
    // 初始化socket
    s->id = id;
    s->fd = fd;
    s->reading = true;
    s->writing = false;
    s->closing = false;
    // id移高16位,低位置0
    ATOM_INIT(&s->sending, ID_TAG16(id) << 16 | 0);
    s->protocol = protocol;
    s->p.size = MIN_READ_BUFFER;
    s->opaque = opaque;
    s->wb_size = 0;
    s->warn_size = 0;
    check_wb_list(&s->high);
    check_wb_list(&s->low);
    s->dw_buffer = NULL;
    s->dw_size = 0;
    // 统计数据置0
    memset(&s->stat, 0, sizeof(s->stat));
    // return 1 代表错误
    // 根据传进来的值,初始化是否监听读事件
    if (enable_read(ss, s, reading)) {
        // 无效化
        ATOM_STORE(&s->type, SOCKET_TYPE_INVALID);
        return NULL;
    }
    return s;
}

static inline void
stat_read(struct socket_server *ss, struct socket *s, int n) {
    s->stat.read += n;
    s->stat.rtime = ss->time;
}

static inline void
stat_write(struct socket_server *ss, struct socket *s, int n) {
    s->stat.write += n;
    s->stat.wtime = ss->time;
}

// return -1 when connecting
// -1 表示还在连接中
static int
open_socket(struct socket_server *ss, struct request_open *request, struct socket_message *result) {
    int id = request->id;
    result->opaque = request->opaque;
    result->id = id;
    result->ud = 0;
    result->data = NULL;
    struct socket *ns;
    int status;
    struct addrinfo ai_hints;
    struct addrinfo *ai_list = NULL;
    struct addrinfo *ai_ptr = NULL;
    char port[16];
    sprintf(port, "%d", request->port);
    memset(&ai_hints, 0, sizeof(ai_hints));
    // 协议族AF_UNSPEC则意味着函数返回的是适用于指定主机名和服务名且适合任何协议族的地址。
    ai_hints.ai_family = AF_UNSPEC;
    // SOCK_STREAM是基于TCP的，数据传输比较有保障。SOCK_DGRAM是基于UDP的
    ai_hints.ai_socktype = SOCK_STREAM;
    // IPPROTO_TCP 和 IPPROTO_IP代表两种不同的协议,分别代表IP协议族里面的TCP协议和IP协议
    ai_hints.ai_protocol = IPPROTO_TCP;
    // getaddrinfo返回一个或多个addrinfo结构,每个都包含一个可以指定的互联网地址
    // 提供与协议无关的主机名到地址转换的函数
    status = getaddrinfo(request->host, port, &ai_hints, &ai_list);
    if (status != 0) {
        // 失败了
        result->data = (void *) gai_strerror(status);
        goto _failed_getaddrinfo;
    }
    int sock = -1;
    for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next) {
        sock = socket(ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol);
        if (sock < 0) {
            continue;
        }
        // 套接字保活
        socket_keepalive(sock);
        // 非阻塞
        sp_nonblocking(sock);
        // 连接
        status = connect(sock, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
        // 非阻塞的方式来进行连接,返回的结果如果是 -1,EINPROGRESS，那么就代表连接还在进行中
        if (status != 0 && errno != EINPROGRESS) {
            // 出错关闭
            close(sock);
            sock = -1;
            continue;
        }
        // 有一个成功就出去了
        break;
    }

    if (sock < 0) {
        result->data = strerror(errno);
        goto _failed;
    }
    // 绑定fd(sock) 到指定id位置的socket
    ns = new_fd(ss, id, sock, PROTOCOL_TCP, request->opaque, true);
    if (ns == NULL) {
        result->data = "reach skynet socket number limit";
        goto _failed;
    }
    // 创建连接成功
    if (status == 0) {
        // 已连接的
        ATOM_STORE(&ns->type, SOCKET_TYPE_CONNECTED);
        struct sockaddr *addr = ai_ptr->ai_addr;
        void *sin_addr = (ai_ptr->ai_family == AF_INET) ? (void *) &((struct sockaddr_in *) addr)->sin_addr
                                                        : (void *) &((struct sockaddr_in6 *) addr)->sin6_addr;
        // 将 IPv4 和 IPv6 地址从二进制转换为文本形式,结果存在ss->buffer
        if (inet_ntop(ai_ptr->ai_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
            result->data = ss->buffer;
        }
        freeaddrinfo(ai_list);
        return SOCKET_OPEN;
    } else {
        // 还在连接中,注册写监听失败,还没连接成功
        if (enable_write(ss, ns, true)) {
            result->data = "enable write failed";
            goto _failed;
        }
        // 注册写监听成功
        ATOM_STORE(&ns->type, SOCKET_TYPE_CONNECTING);
    }

    freeaddrinfo(ai_list);
    return -1;
    _failed:
    if (sock >= 0)
        close(sock);
    freeaddrinfo(ai_list);
    _failed_getaddrinfo:
    // socket 无效化
    ATOM_STORE(&ss->slot[HASH_ID(id)].type, SOCKET_TYPE_INVALID);
    return SOCKET_ERR;
}

/**
 * 存error信息到result
 * @param s
 * @param result
 * @param err
 * @return
 */
static int
report_error(struct socket *s, struct socket_message *result, const char *err) {
    result->id = s->id;
    result->ud = 0;
    result->opaque = s->opaque;
    result->data = (char *) err;
    return SOCKET_ERR;
}

static int
close_write(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
    // 已经在关闭中
    if (s->closing) {
        // 执行强行close
        force_close(ss, s, l, result);
        return SOCKET_RST;
    } else {
        // socket的类型
        int t = ATOM_LOAD(&s->type);
        // 半关闭读
        if (t == SOCKET_TYPE_HALFCLOSE_READ) {
            // recv 0 before, ignore the error and close fd
            // 忽略错误,执行关闭
            force_close(ss, s, l, result);
            return SOCKET_RST;
        }
        // 半关闭写
        if (t == SOCKET_TYPE_HALFCLOSE_WRITE) {
            // already raise SOCKET_ERR
            // 已经引发SOCKET_ERR
            return SOCKET_RST;
        }
        // 普通模式下未进行关闭时,设置模式为半关闭写
        ATOM_STORE(&s->type, SOCKET_TYPE_HALFCLOSE_WRITE);
        shutdown(s->fd, SHUT_WR);
        // 关闭可写事件监听
        enable_write(ss, s, false);
        // 记录异常结果到result中
        return report_error(s, result, strerror(errno));
    }
}

static int
send_list_tcp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_lock *l,
              struct socket_message *result) {
    while (list->head) {
        struct write_buffer *tmp = list->head;
        for (;;) {
            ssize_t sz = write(s->fd, tmp->ptr, tmp->sz);
            // 没写入成功
            if (sz < 0) {
                switch (errno) {
                    // 中断,由于信号中断，没写成功任何数据,跳出再次尝试写入
                    case EINTR:
                        continue;
                        //EAGAIN 资源短暂不可用，这个操作可能等下重试后可用。它的另一个名字叫做EWOULDAGAIN，这两个宏定义在GNU的c库中永远是同一个值
                        // 有些较老的unix系统上，这两个值的意义可能不一样
                        // 为了使程序可移植，应该检查这两个代码并将它们视为相同。
                    case AGAIN_WOULDBLOCK:
                        // 暂时不可写入,下次继续尝试
                        return -1;
                }
                // 除AGAIN_WOULDBLOCK 情况外,其它错误都关闭连接
                return close_write(ss, s, l, result);
            }
            // 写入成功,统计socket的写入数据
            stat_write(ss, s, (int) sz);
            s->wb_size -= sz;
            // 当前包没发送完,发送缓冲区又写不下了。
            if (sz != tmp->sz) {
                tmp->ptr += sz;
                tmp->sz -= sz;
                return -1;
            }
            // 写完当前包,证明发送缓冲区还没写满,继续后续的逻辑
            break;
        }
        // 写入成功,调整待发送列表头
        list->head = tmp->next;
        write_buffer_free(ss, tmp);
    }
    // 发送完毕后,tail置空
    list->tail = NULL;
    return -1;
}

static socklen_t
udp_socket_address(struct socket *s, const uint8_t udp_address[UDP_ADDRESS_SIZE], union sockaddr_all *sa) {
    int type = (uint8_t) udp_address[0];
    // udp 类型不匹配
    if (type != s->protocol)
        return 0;
    uint16_t port = 0;
    // 端口
    memcpy(&port, udp_address + 1, sizeof(uint16_t));
    switch (s->protocol) {
        // ipv4
        case PROTOCOL_UDP:
            memset(&sa->v4, 0, sizeof(sa->v4));
            sa->s.sa_family = AF_INET;
            sa->v4.sin_port = port;
            // ip
            memcpy(&sa->v4.sin_addr, udp_address + 1 + sizeof(uint16_t),
                   sizeof(sa->v4.sin_addr));    // ipv4 address is 32 bits
            return sizeof(sa->v4);
            // ipv6
        case PROTOCOL_UDPv6:
            memset(&sa->v6, 0, sizeof(sa->v6));
            sa->s.sa_family = AF_INET6;
            sa->v6.sin6_port = port;
            // ip
            memcpy(&sa->v6.sin6_addr, udp_address + 1 + sizeof(uint16_t),
                   sizeof(sa->v6.sin6_addr)); // ipv6 address is 128 bits
            return sizeof(sa->v6);
    }
    return 0;
}

static void
drop_udp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct write_buffer *tmp) {
    // 从列表中删除当前发送的tmp
    s->wb_size -= tmp->sz;
    list->head = tmp->next;
    if (list->head == NULL)
        list->tail = NULL;
    // 释放tmp
    write_buffer_free(ss, tmp);
}

/*
 * UDP发送待写入列表
 */
static int
send_list_udp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
    while (list->head) {
        struct write_buffer *tmp = list->head;
        struct write_buffer_udp *udp = (struct write_buffer_udp *) tmp;
        union sockaddr_all sa;
        // 填充 udp_address,这里只是为了计算下协议相关填充长度的感觉
        socklen_t sasz = udp_socket_address(s, udp->udp_address, &sa);
        if (sasz == 0) {
            skynet_error(NULL, "socket-server : udp (%d) type mismatch.", s->id);
            // 发送失败,直接丢,从列表中删除当前发送的tmp
            drop_udp(ss, s, list, tmp);
            return -1;
        }
        // 发送数据
        int err = sendto(s->fd, tmp->ptr, tmp->sz, 0, &sa.s, sasz);
        if (err < 0) {
            switch (errno) {
                // 中断
                case EINTR:
                    // 发送失败
                case AGAIN_WOULDBLOCK:
                    return -1;
            }
            skynet_error(NULL, "socket-server : udp (%d) sendto error %s.", s->id, strerror(errno));
            // 其它错误,直接丢弃发送信息
            drop_udp(ss, s, list, tmp);
            return -1;
        }
        // 统计成功写入的数据
        stat_write(ss, s, tmp->sz);
        s->wb_size -= tmp->sz;
        list->head = tmp->next;
        // 释放已发送数据
        write_buffer_free(ss, tmp);
    }
    // 发送完毕后,tail置空
    list->tail = NULL;
    return -1;
}

static int
send_list(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_lock *l,
          struct socket_message *result) {
    // 根据tcp还是udp去调用
    if (s->protocol == PROTOCOL_TCP) {
        return send_list_tcp(ss, s, list, l, result);
    } else {
        return send_list_udp(ss, s, list, result);
    }
}

static inline int
list_uncomplete(struct wb_list *s) {
    struct write_buffer *wb = s->head;
    if (wb == NULL)
        return 0;

    return (void *) wb->ptr != wb->buffer;
}

static void
raise_uncomplete(struct socket *s) {
    struct wb_list *low = &s->low;
    struct write_buffer *tmp = low->head;
    low->head = tmp->next;
    if (low->head == NULL) {
        low->tail = NULL;
    }

    // move head of low list (tmp) to the empty high list
    struct wb_list *high = &s->high;
    assert(high->head == NULL);
    // 将低列表的头移到空的高列表
    tmp->next = NULL;
    high->head = high->tail = tmp;
}

static inline int
send_buffer_empty(struct socket *s) {
    return (s->high.head == NULL && s->low.head == NULL);
}

/*
	Each socket has two write buffer list, high priority and low priority.

	1. send high list as far as possible.
	2. If high list is empty, try to send low list.
	3. If low list head is uncomplete (send a part before), move the head of low list to empty high list (call raise_uncomplete) .
	4. If two lists are both empty, turn off the event. (call check_close)
 */
/*
 * 每个套接字有两个写缓冲区列表，高优先级和低优先级。
 * 1.尽可能发送高优先级列表。
 * 2.如果高优先级列表为空，尝试发送低优先级列表。
 * 3.如果低优先级列表头不完整（之前发送一部分），则将低列表的头移动到空的高优先级列表（调用raise_incompete）。
 * 4.如果两个列表都为空，请关闭该事件。（调用check_close）
 */
static int
send_buffer_(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
    assert(!list_uncomplete(&s->low));
    // step 1
    // 发送高优先级列表
    int ret = send_list(ss, s, &s->high, l, result);
    // 正常发送流程返回-1
    if (ret != -1) {
        // 设置模式为半关闭写,会返回SOCKET_ERR
        if (ret == SOCKET_ERR) {
            // HALFCLOSE_WRITE
            return SOCKET_ERR;
        }
        // SOCKET_RST (ignore)
        // 忽略SOCKET_RST,已经关闭了
        return -1;
    }
    // 高优先级列表发送完
    if (s->high.head == NULL) {
        // step 2
        // 发送低优先级,一样的处理 
        if (s->low.head != NULL) {
            int ret = send_list(ss, s, &s->low, l, result);
            if (ret != -1) {
                if (ret == SOCKET_ERR) {
                    // HALFCLOSE_WRITE
                    return SOCKET_ERR;
                }
                // SOCKET_RST (ignore)
                return -1;
            }
            // step 3
            // 如果低优先级列表没有发送完
            if (list_uncomplete(&s->low)) {
                // 只将低优先级列表的头移到高优先级列表中去(这时候高优先级一定是空)
                raise_uncomplete(s);
                // 返回
                return -1;
            }
            // 如果上面发送完低优先级列表,则结束返回
            if (s->low.head)
                return -1;
        }
        // 低优先级列表也为空
        // step 4
        assert(send_buffer_empty(s) && s->wb_size == 0);

        if (s->closing) {
            // finish writing
            // 如果socket 正在关闭,关闭它,此时数据已经完全发送结束了
            force_close(ss, s, l, result);
            return -1;
        }
        // 全部数据返送完成,关闭可写监听
        int err = enable_write(ss, s, false);
        // error
        if (err) {
            return report_error(s, result, "disable write failed");
        }
        // 警告信息
        if (s->warn_size > 0) {
            s->warn_size = 0;
            result->opaque = s->opaque;
            result->id = s->id;
            result->ud = 0;
            result->data = NULL;
            return SOCKET_WARNING;
        }
    }

    return -1;
}

static int
send_buffer(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
    // 加锁失败,同时只能一个线程来写
    if (!socket_trylock(l))
        return -1;    // blocked by direct write, send later.

    // dwbuffer(直接写入缓冲区) 不为空
    if (s->dw_buffer) {
        // add direct write buffer before high.head
        // 将要写入的buffer,write_buffer结构体是一个链表,用来存放写缓冲区满,不可写入的数据。待可写事件发生时,继续写入。
        struct write_buffer *buf = MALLOC(sizeof(*buf));
        struct send_object so;
        // 初始化传进来的数据,这里so初始化好像没什么意义,会走一遍ss.soi里面的方法
        buf->userobject = send_object_init(ss, &so, (void *) s->dw_buffer, s->dw_size);
        buf->ptr = (char *) so.buffer + s->dw_offset;
        buf->sz = so.sz - s->dw_offset;
        buf->buffer = (void *) s->dw_buffer;
        s->wb_size += buf->sz;
        // wbufferlist,待发送数据链表高优先级
        if (s->high.head == NULL) {
            s->high.head = s->high.tail = buf;
            buf->next = NULL;
        } else {
            // 头插
            buf->next = s->high.head;
            s->high.head = buf;
        }
        // 清空直接写入缓冲区
        s->dw_buffer = NULL;
    }
    // 开始发送待发送列表
    int r = send_buffer_(ss, s, l, result);
    socket_unlock(l);
    return r;
}

static struct write_buffer *
append_sendbuffer_(struct socket_server *ss, struct wb_list *s, struct request_send *request, int size) {
    struct write_buffer *buf = MALLOC(size);
    struct send_object so;
    // 初始化发送obj,记录是否用户数据
    buf->userobject = send_object_init(ss, &so, request->buffer, request->sz);
    buf->ptr = (char *) so.buffer;
    buf->sz = so.sz;
    buf->buffer = request->buffer;
    buf->next = NULL;
    // 尾插
    if (s->head == NULL) {
        s->head = s->tail = buf;
    } else {
        assert(s->tail != NULL);
        assert(s->tail->next == NULL);
        s->tail->next = buf;
        s->tail = buf;
    }
    return buf;
}

static inline void
append_sendbuffer_udp(struct socket_server *ss, struct socket *s, int priority, struct request_send *request,
                      const uint8_t udp_address[UDP_ADDRESS_SIZE]) {
    struct wb_list *wl = (priority == PRIORITY_HIGH) ? &s->high : &s->low;
    struct write_buffer_udp *buf = (struct write_buffer_udp *) append_sendbuffer_(ss, wl, request, sizeof(*buf));
    memcpy(buf->udp_address, udp_address, UDP_ADDRESS_SIZE);
    s->wb_size += buf->buffer.sz;
}

static inline void
append_sendbuffer(struct socket_server *ss, struct socket *s, struct request_send *request) {
    // 追加到高优先级待发送列表
    struct write_buffer *buf = append_sendbuffer_(ss, &s->high, request, sizeof(*buf));
    // 增加等待写入缓冲区大小
    s->wb_size += buf->sz;
}

static inline void
append_sendbuffer_low(struct socket_server *ss, struct socket *s, struct request_send *request) {
    struct write_buffer *buf = append_sendbuffer_(ss, &s->low, request, sizeof(*buf));
    s->wb_size += buf->sz;
}

static int
trigger_write(struct socket_server *ss, struct request_send *request, struct socket_message *result) {
    int id = request->id;
    struct socket *s = &ss->slot[HASH_ID(id)];
    if (socket_invalid(s, id))
        return -1;
    if (enable_write(ss, s, true)) {
        return report_error(s, result, "enable write failed");
    }
    return -1;
}

/*
	When send a package , we can assign the priority : PRIORITY_HIGH or PRIORITY_LOW

	If socket buffer is empty, write to fd directly.
		If write a part, append the rest part to high list. (Even priority is PRIORITY_LOW)
	Else append package to high (PRIORITY_HIGH) or low (PRIORITY_LOW) list.
 */
/*
 发送包时，我们可以指定优先级：PRIORITY_HIGH或PRIORITY_LOW。
 如果socket缓冲区为空，则直接写入fd。
 如果写了一部分，则将其余部分追加到高列表中（即使优先级是 PRIORITY_LOW）
 否则将包附加到高 (PRIORITY_HIGH) 或低 (PRIORITY_LOW) 列表
 */
/**
 * socket发送
 * @param ss
 * @param request 请求发送信息
 * @param result 保存结果
 * @param priority 指示发送高优先级列表还是低优先级列表
 * @param udp_address 发送为udp时,对方的udp地址信息
 * @return
 */
static int
send_socket(struct socket_server *ss, struct request_send *request, struct socket_message *result, int priority,
            const uint8_t *udp_address) {
    int id = request->id;
    struct socket *s = &ss->slot[HASH_ID(id)];
    struct send_object so;
    // 初始化要发送的数据,如果是用户数据,可以再这里定制化
    send_object_init(ss, &so, request->buffer, request->sz);
    uint8_t type = ATOM_LOAD(&s->type);
    // socket下面这几种情况都不可写
    if (type == SOCKET_TYPE_INVALID || s->id != id
        || type == SOCKET_TYPE_HALFCLOSE_WRITE
        || type == SOCKET_TYPE_PACCEPT
        || s->closing) {
        // 释放buffer
        so.free_func((void *) request->buffer);
        return -1;
    }
    // 监听socket,不能发送
    if (type == SOCKET_TYPE_PLISTEN || type == SOCKET_TYPE_LISTEN) {
        skynet_error(NULL, "socket-server: write to listen fd %d.", id);
        so.free_func((void *) request->buffer);
        return -1;
    }
    // socket的两个待发送列表都为空
    if (send_buffer_empty(s)) {
        // 如果是TCP协议,追加到列表
        if (s->protocol == PROTOCOL_TCP) {
            // 添加到高优先级列表，即使传入的优先级是PRIORITY_LOW
            append_sendbuffer(ss, s, request);    // add to high priority list, even priority == PRIORITY_LOW
        } else {
            // UDP
            // udp地址为null,填充为socket内的地址
            if (udp_address == NULL) {
                udp_address = s->p.udp_address;
            }
            union sockaddr_all sa;
            // 填充upd_addr
            socklen_t sasz = udp_socket_address(s, udp_address, &sa);
            if (sasz == 0) {
                // udp type mismatch, just drop it.
                // udp类型不匹配，直接删除
                skynet_error(NULL, "socket-server: udp socket (%d) type mismatch.", id);
                so.free_func((void *) request->buffer);
                return -1;
            }
            // 直接发送数据
            int n = sendto(s->fd, so.buffer, so.sz, 0, &sa.s, sasz);
            if (n != so.sz) {
                // 发送失败,追加到发送列表中去,这里会把udp_address存到消息体中去
                append_sendbuffer_udp(ss, s, priority, request, udp_address);
            } else {
                // 统计写入
                stat_write(ss, s, n);
                so.free_func((void *) request->buffer);
                return -1;
            }
        }
        // 开启监听写事件
        if (enable_write(ss, s, true)) {
            return report_error(s, result, "enable write failed");
        }
    } else {
        // 待发送列表不为空,直接尾插
        if (s->protocol == PROTOCOL_TCP) {
            if (priority == PRIORITY_LOW) {
                append_sendbuffer_low(ss, s, request);
            } else {
                append_sendbuffer(ss, s, request);
            }
        } else {
            // 请求数据没有带uod_addr,就用socket的填充
            if (udp_address == NULL) {
                udp_address = s->p.udp_address;
            }
            append_sendbuffer_udp(ss, s, priority, request, udp_address);
        }
    }
    if (s->wb_size >= WARNING_SIZE && s->wb_size >= s->warn_size) {
        // 超过警戒线就两倍扩张
        s->warn_size = s->warn_size == 0 ? WARNING_SIZE * 2 : s->warn_size * 2;
        result->opaque = s->opaque;
        result->id = s->id;
        result->ud = s->wb_size % 1024 == 0 ? s->wb_size / 1024 : s->wb_size / 1024 + 1;
        result->data = NULL;
        return SOCKET_WARNING;
    }
    return -1;
}

static int
listen_socket(struct socket_server *ss, struct request_listen *request, struct socket_message *result) {
    int id = request->id;
    int listen_fd = request->fd;
    // 绑定fd到socket,初始化读监听false
    struct socket *s = new_fd(ss, id, listen_fd, PROTOCOL_TCP, request->opaque, false);
    if (s == NULL) {
        goto _failed;
    }
    // P_LISTEN  暂停监听
    ATOM_STORE(&s->type, SOCKET_TYPE_PLISTEN);
    result->opaque = request->opaque;
    result->id = id;
    result->ud = 0;
    result->data = "listen";
    // addr联合体
    union sockaddr_all u;
    socklen_t slen = sizeof(u);
    // 没有名字
    if (getsockname(listen_fd, &u.s, &slen) == 0) {
        // AF_INET ipv4协议族
        // 拿到地址
        void *sin_addr = (u.s.sa_family == AF_INET) ? (void *) &u.v4.sin_addr : (void *) &u.v6.sin6_addr;
        // 将 IPv4 和 IPv6 地址从二进制转换为文本形式,结果存在ss->buffer
        if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer)) == 0) {
            result->data = strerror(errno);
            return SOCKET_ERR;
        }
        // 端口
        int sin_port = ntohs((u.s.sa_family == AF_INET) ? u.v4.sin_port : u.v6.sin6_port);
        result->data = ss->buffer;
        result->ud = sin_port;
    } else {
        result->data = strerror(errno);
        return SOCKET_ERR;
    }

    return SOCKET_OPEN;
    _failed:
    // 绑定失败
    close(listen_fd);
    result->opaque = request->opaque;
    result->id = id;
    result->ud = 0;
    result->data = "reach skynet socket number limit"; // 达到套接字数量限制”
    // 绑定失败的socket无效化
    ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;

    return SOCKET_ERR;
}

static inline int
nomore_sending_data(struct socket *s) {
    return (send_buffer_empty(s) && s->dw_buffer == NULL && (ATOM_LOAD(&s->sending) & 0xffff) == 0)
           || (ATOM_LOAD(&s->type) == SOCKET_TYPE_HALFCLOSE_WRITE);
}

static void
close_read(struct socket_server *ss, struct socket *s, struct socket_message *result) {
    // Don't read socket later
    ATOM_STORE(&s->type, SOCKET_TYPE_HALFCLOSE_READ);
    enable_read(ss, s, false);
    shutdown(s->fd, SHUT_RD);
    result->id = s->id;
    result->ud = 0;
    result->data = NULL;
    result->opaque = s->opaque;
}

// 1）shutdown可携带一个参数，取值有3个，分别意味着：只关闭读、只关闭写、同时关闭读写。
//    对于监听句柄，如果参数为关闭写，显然没有任何意义。但关闭读从某方面来说是有意义的，例如不再接受新的连接。看看最右边蓝色分支，针对监听句柄，若参数为关闭写，则不做任何事；若为关闭读，则把端口上的半打开连接使用RST关闭，与close如出一辙。
// 2）若shutdown的是半打开的连接，则发出RST来关闭连接。
// 3）若shutdown的是正常连接，那么关闭读其实与对端是没有关系的。只要本机把接收掉的消息丢掉，其实就等价于关闭读了，并不一定非要对端关闭写的。实际上，shutdown正是这么干的。若参数中的标志位含有关闭读，只是标识下，当我们调用read等方法时这个标识就起作用了，会使进程读不到任何数据。
// 4）若参数中有标志位为关闭写，那么下面做的事与close是一致的：发出FIN包，告诉对方，本机不会再发消息了。

//
// 半关闭 shutdown 函数,其原型为：
// int shutdown(int sockfd,int howto)
// 对已连接的套接字执行 shutdown 操作，若成功则为 0，若出错则为 -1。
// 
// 第二个参数 howto 是这个函数的设置选项，它的设置有三个主要选项：
// SHUT_RD(0)：关闭连接的“读”这个方向，对该套接字进行读操作直接返回 EOF。
// 从数据角度来看，套接字上接收缓冲区已有的数据将被丢弃，如果再有新的数据流到达，会对数据进行 ACK，然后悄悄地丢弃。
// 也就是说，对端还是会接收到 ACK，在这种情况下根本不知道数据已经被丢弃了。
//
// SHUT_WR(1)：关闭连接的“写”这个方向，这就是常被称为”半关闭“的连接。
// 此时，不管套接字引用计数的值是多少，都会直接关闭连接的写方向。
// 套接字上发送缓冲区已有的数据将被立即发送出去，并发送一个 FIN 报文给对端。应用程序如果对该套接字进行写操作会报错。
//
// SHUT_RDWR(2)：相当于 SHUT_RD 和 SHUT_WR 操作各一次，关闭套接字的读和写两个方向。

static inline int
halfclose_read(struct socket *s) {
    return ATOM_LOAD(&s->type) == SOCKET_TYPE_HALFCLOSE_READ;
}

// SOCKET_CLOSE can be raised (only once) in one of two conditions.
// See https://github.com/cloudwu/skynet/issues/1346 for more discussion.
// 1. close socket by self, See close_socket()
// 2. recv 0 or eof event (close socket by remote), See forward_message_tcp()
// It's able to write data after SOCKET_CLOSE (In condition 2), but if remote is closed, SOCKET_ERR may raised.

// 在以下两种情况之一下可以引发 SOCKET_CLOSE（仅一次）
// 1. 自行关闭socket，参见close_socket()
// 2. recv 0 或 eof 事件（通过远程关闭套接字），请参阅forward_message_tcp()
// 它能够在 SOCKET_CLOSE 之后写入数据（在条件 2 中），但如果远程关闭，则可能会引发 SOCKET_ERR
static int
close_socket(struct socket_server *ss, struct request_close *request, struct socket_message *result) {
    int id = request->id;
    struct socket *s = &ss->slot[HASH_ID(id)];
    // 套接字已关闭
    if (socket_invalid(s, id)) {
        // The socket is closed, ignore
        return -1;
    }
    struct socket_lock l;
    socket_lock_init(s, &l);

    // 半关闭读
    int shutdown_read = halfclose_read(s);
    // 请求是shutdown or  已经不会再发送数据(待发送数据为空 || 已经半关闭写)
    if (request->shutdown || nomore_sending_data(s)) {
        // If socket is SOCKET_TYPE_HALFCLOSE_READ, Do not raise SOCKET_CLOSE again.
        int r = shutdown_read ? -1 : SOCKET_CLOSE;
        // 强制关闭
        force_close(ss, s, &l, result);
        return r;
    }
    // 关闭标志
    s->closing = true;
    // 没有半关闭读就关掉它
    if (!shutdown_read) {
        // don't read socket after socket.close()
        // 半关闭读
        close_read(ss, s, result);
        return SOCKET_CLOSE;
    }
    // recv 0 before (socket is SOCKET_TYPE_HALFCLOSE_READ) and waiting for sending data out.
    // 半关闭读且还可以发送数据
    return -1;
}

/**
 * 绑定fd到socket,socket是抽象出来的,fd是底层对应的连接
 * @param ss
 * @param request 消息体
 * @param result
 * @return
 */
static int
bind_socket(struct socket_server *ss, struct request_bind *request, struct socket_message *result) {
    int id = request->id;
    result->id = id;
    result->opaque = request->opaque;
    result->ud = 0;
    struct socket *s = new_fd(ss, id, request->fd, PROTOCOL_TCP, request->opaque, true);
    if (s == NULL) {
        // 绑定失败
        result->data = "reach skynet socket number limit";
        return SOCKET_ERR;
    }
    // 设置fd为非阻塞
    sp_nonblocking(request->fd);
    // socket已绑定fd
    ATOM_STORE(&s->type, SOCKET_TYPE_BIND);
    result->data = "binding";
    return SOCKET_OPEN;
}

/**
 * 恢复暂停
 * @param ss
 * @param request 消息体
 * @param result
 * @return
 */
static int
resume_socket(struct socket_server *ss, struct request_resumepause *request, struct socket_message *result) {
    int id = request->id;
    result->id = id;
    result->opaque = request->opaque;
    result->ud = 0;
    result->data = NULL;
    struct socket *s = &ss->slot[HASH_ID(id)];
    if (socket_invalid(s, id)) {
        result->data = "invalid socket";
        return SOCKET_ERR;
    }
    // 半关闭读,已经不接受数据了, 恢复也就没用了
    if (halfclose_read(s)) {
        // The closing socket may be in transit, so raise an error. See https://github.com/cloudwu/skynet/issues/1374
        // 关闭socket可能正在运输中，因此引发错误
        result->data = "socket closed";
        return SOCKET_ERR;
    }
    struct socket_lock l;
    socket_lock_init(s, &l);
    // 开启读事件监听
    if (enable_read(ss, s, true)) {
        // 开启失败(1 已开启读监听;2 注册读监听失败)
        result->data = "enable read failed";
        return SOCKET_ERR;
    }
    uint8_t type = ATOM_LOAD(&s->type);
    // 暂停接收 || 暂停监听
    if (type == SOCKET_TYPE_PACCEPT || type == SOCKET_TYPE_PLISTEN) {
        //  暂停接收 -> 连接        暂停监听 -> 监听
        ATOM_STORE(&s->type, (type == SOCKET_TYPE_PACCEPT) ? SOCKET_TYPE_CONNECTED : SOCKET_TYPE_LISTEN);
        s->opaque = request->opaque;
        result->data = "start";
        return SOCKET_OPEN;
    } else if (type == SOCKET_TYPE_CONNECTED) {
        // 是连接状态的情况
        // todo: maybe we should send a message SOCKET_TRANSFER to s->opaque
        // 也许我们应该向s->opaque发送一条消息SOCKET_TRANSFER
        s->opaque = request->opaque;
        result->data = "transfer";
        return SOCKET_OPEN;
    }
    // if s->type == SOCKET_TYPE_HALFCLOSE_WRITE , SOCKET_CLOSE message will send later
    // 如果是半关闭写 || 关闭, 将稍后发送(这里不处理)
    return -1;
}

/**
 * 暂停
 * @param ss
 * @param request 消息体
 * @param result
 * @return
 */
static int
pause_socket(struct socket_server *ss, struct request_resumepause *request, struct socket_message *result) {
    int id = request->id;
    struct socket *s = &ss->slot[HASH_ID(id)];
    // 目标socket已失效
    if (socket_invalid(s, id)) {
        return -1;
    }
    // 关闭读监听
    if (enable_read(ss, s, false)) {
        return report_error(s, result, "enable read failed");
    }
    return -1;
}

/**
 * 设置选项
 * @param ss
 * @param request
 */
static void
setopt_socket(struct socket_server *ss, struct request_setopt *request) {
    int id = request->id;
    struct socket *s = &ss->slot[HASH_ID(id)];
    if (socket_invalid(s, id)) {
        return;
    }
    int v = request->value;
    // IPPROTO_TCP 和 IPPROTO_IP代表两种不同的协议,分别代表IP协议族里面的TCP协议和IP协议
    setsockopt(s->fd, IPPROTO_TCP, request->what, &v, sizeof(v));
}

static void
block_readpipe(int pipefd, void *buffer, int sz) {
    for (;;) {
        // 对管道套接字执行 read 操作是一个原子操作，不会被别的情况比如信号之类的打断
        // 所以只有两种可能，错误和全部读取完成
        int n = read(pipefd, buffer, sz);
        if (n < 0) {
            // 读取被中断 再次尝试
            if (errno == EINTR)
                continue;
            // 其它错误
            skynet_error(NULL, "socket-server : read pipe error %s.", strerror(errno));
            return;
        }
        // must atomic read from a pipe
        // 原子读取
        assert(n == sz);
        return;
    }
}

static int
has_cmd(struct socket_server *ss) {
    struct timeval tv = {0, 0};
    int retval;
    // FD_SET(int fd, fd_set *fdset);       //将fd加入set集合
    // FD_CLR(int fd, fd_set *fdset);       //将fd从set集合中清除
    // FD_ISSET(int fd, fd_set *fdset);     //检测fd是否在set集合中，不在则返回0
    // FD_ZERO(fd_set *fdset);              //将set清零使集合中不含任何fd
    // 将接受socketFD  加入fd_set中去
    FD_SET(ss->recvctrl_fd, &ss->rfds);
    // int select(int nfds,  fd_set* readset,  fd_set* writeset,  fe_set* exceptset,  struct timeval* timeout);
    //       nfds           需要检查的文件描述字个数
    //       readset     用来检查可读性的一组文件描述字。
    //       writeset     用来检查可写性的一组文件描述字。
    //       exceptset  用来检查是否有异常条件出现的文件描述字。(注：错误不包括在异常条件之内)
    //       timeout      超时，填NULL为阻塞，填0为非阻塞，其他为一段超时时间
    retval = select(ss->recvctrl_fd + 1, &ss->rfds, NULL, NULL, &tv);
    if (retval == 1) {
        return 1;
    }
    return 0;
}

static void
add_udp_socket(struct socket_server *ss, struct request_udp *udp) {
    int id = udp->id;
    int protocol;
    if (udp->family == AF_INET6) {
        protocol = PROTOCOL_UDPv6;
    } else {
        protocol = PROTOCOL_UDP;
    }
    // 绑定fd到socket
    struct socket *ns = new_fd(ss, id, udp->fd, protocol, udp->opaque, true);
    if (ns == NULL) {
        // 关闭
        close(udp->fd);
        ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
        return;
    }
    ATOM_STORE(&ns->type, SOCKET_TYPE_CONNECTED);
    // socket清除udp目标地址的旧数据
    memset(ns->p.udp_address, 0, sizeof(ns->p.udp_address));
}

static int
set_udp_address(struct socket_server *ss, struct request_setudp *request, struct socket_message *result) {
    int id = request->id;
    struct socket *s = &ss->slot[HASH_ID(id)];
    if (socket_invalid(s, id)) {
        return -1;
    }
    int type = request->address[0];
    if (type != s->protocol) {
        // protocol mismatch
        return report_error(s, result, "protocol mismatch");
    }
    // ipv4
    if (type == PROTOCOL_UDP) {
        memcpy(s->p.udp_address, request->address, 1 + 2 + 4);    // 1 type, 2 port, 4 ipv4
    } else {
        // ipv6
        memcpy(s->p.udp_address, request->address, 1 + 2 + 16);    // 1 type, 2 port, 16 ipv6
    }
    // -1;
    ATOM_FDEC(&s->udpconnecting);
    return -1;
}

static inline void
inc_sending_ref(struct socket *s, int id) {
    if (s->protocol != PROTOCOL_TCP)
        return;
    for (;;) {
        unsigned long sending = ATOM_LOAD(&s->sending);
        if ((sending >> 16) == ID_TAG16(id)) {
            if ((sending & 0xffff) == 0xffff) {
                // s->sending may overflow (rarely), so busy waiting here for socket thread dec it. see issue #794
                continue;
            }
            // inc sending only matching the same socket id
            if (ATOM_CAS_ULONG(&s->sending, sending, sending + 1))
                return;
            // atom inc failed, retry
        } else {
            // socket id changed, just return
            return;
        }
    }
}

static inline void
dec_sending_ref(struct socket_server *ss, int id) {
    struct socket *s = &ss->slot[HASH_ID(id)];
    // Notice: udp may inc sending while type == SOCKET_TYPE_RESERVE
    // udp可能会在类型为SOCKET_TYPE_RESERVE时发送
    // 限制为TCP,
    if (s->id == id && s->protocol == PROTOCOL_TCP) {
        assert((ATOM_LOAD(&s->sending) & 0xffff) != 0);
        // sending -= 1
        ATOM_FDEC(&s->sending);
    }
}

// return type
static int
ctrl_cmd(struct socket_server *ss, struct socket_message *result) {
    int fd = ss->recvctrl_fd;
    // the length of message is one byte, so 256 buffer size is enough.
    // 消息的长度是一个字节，所以256个缓冲区大小就足够了。
    uint8_t buffer[256];
    uint8_t header[2];
    // 读取fd接收的数据到header
    block_readpipe(fd, header, sizeof(header));
    // 消息类型
    int type = header[0];
    // 消息长度
    int len = header[1];
    // 根据消息长度读取消息体
    block_readpipe(fd, buffer, len);
    // ctrl command only exist in local fd, so don't worry about endian.
    // ctrl命令只存在于本地fd中，所以不用担心字节序(大小端)
    switch (type) {
        case 'R':
            // 恢复socket
            return resume_socket(ss, (struct request_resumepause *) buffer, result);
        case 'S':
            // 暂停
            return pause_socket(ss, (struct request_resumepause *) buffer, result);
        case 'B':
            // 绑定
            return bind_socket(ss, (struct request_bind *) buffer, result);
        case 'L':
            // 监听
            return listen_socket(ss, (struct request_listen *) buffer, result);
        case 'K':
            // 关闭
            return close_socket(ss, (struct request_close *) buffer, result);
        case 'O':
            // open
            return open_socket(ss, (struct request_open *) buffer, result);
        case 'X':
            result->opaque = 0;
            result->id = 0;
            result->ud = 0;
            result->data = NULL;
            // 退出
            return SOCKET_EXIT;
        case 'W':
            // 开启写监听
            return trigger_write(ss, (struct request_send *) buffer, result);
        case 'D':
        case 'P': {
            int priority = (type == 'D') ? PRIORITY_HIGH : PRIORITY_LOW;
            struct request_send *request = (struct request_send *) buffer;
            int ret = send_socket(ss, request, result, priority, NULL);
            // 减少发送计数引用
            dec_sending_ref(ss, request->id);
            return ret;
        }
        case 'A': {
            // udp发送
            struct request_send_udp *rsu = (struct request_send_udp *) buffer;
            return send_socket(ss, &rsu->send, result, PRIORITY_HIGH, rsu->address);
        }
        case 'C':
            // 设置udp的地址
            return set_udp_address(ss, (struct request_setudp *) buffer, result);
        case 'T':
            // 设置tcp选项
            setopt_socket(ss, (struct request_setopt *) buffer);
            return -1;
        case 'U':
            // 添加udp连接
            add_udp_socket(ss, (struct request_udp *) buffer);
            return -1;
        default:
            // 打印错误信息
            skynet_error(NULL, "socket-server: Unknown ctrl %c.", type);
            return -1;
    };

    return -1;
}

// return -1 (ignore) when error
// 出错时返回-1（忽略）
static int
forward_message_tcp(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
    int sz = s->p.size;
    char *buffer = MALLOC(sz);
    int n = (int) read(s->fd, buffer, sz);
    // 读取失败
    if (n < 0) {
        FREE(buffer);
        switch (errno) {
            case EINTR:
            case AGAIN_WOULDBLOCK:
                break;
            default:
                return report_error(s, result, strerror(errno));
        }
        return -1;
    }
    if (n == 0) {
        FREE(buffer);
        if (s->closing) {
            // Rare case : if s->closing is true, reading event is disable, and SOCKET_CLOSE is raised.
            // 罕见情况：如果 s-> closing 为 true，则禁用读取事件，并提前SOCKET_CLOSE。
            if (nomore_sending_data(s)) {
                force_close(ss, s, l, result);
            }
            return -1;
        }
        int t = ATOM_LOAD(&s->type);
        if (t == SOCKET_TYPE_HALFCLOSE_READ) {
            // Rare case : Already shutdown read.
            // 罕见情况：已关闭读取
            return -1;
        }
        if (t == SOCKET_TYPE_HALFCLOSE_WRITE) {
            // Remote shutdown read (write error) before.
            // 远程关机之前读取（写入错误）。
            force_close(ss, s, l, result);
        } else {
            // 为什么这里要关闭读,n == 0
            close_read(ss, s, result);
        }
        return SOCKET_CLOSE;
    }
    // 如果是半关闭读,半关闭读的情况就是把接收数据直接丢弃
    if (halfclose_read(s)) {
        // discard recv data (Rare case : if socket is HALFCLOSE_READ, reading event is disable.)
        // 丢弃接收数据（罕见情况：如果socket为HALFCLOSE_READ，读取事件被禁用
        FREE(buffer);
        return -1;
    }
    // 正常读取到数据
    // 统计读数据
    stat_read(ss, s, n);

    result->opaque = s->opaque;
    result->id = s->id;
    result->ud = n;
    result->data = buffer;

    // 读取到的数据超过当前size,两倍扩展
    if (n == sz) {
        s->p.size *= 2;
        return SOCKET_MORE;
    } else if (sz > MIN_READ_BUFFER && n * 2 < sz) {
        // 同样2倍的缩容
        s->p.size /= 2;
    }
    return SOCKET_DATA;
}

/**
 * 生成udp地址
 * @param protocol
 * @param sa
 * @param udp_address
 * @return
 */
static int
gen_udp_address(int protocol, union sockaddr_all *sa, uint8_t *udp_address) {
    // 第一位放addr类型
    int addrsz = 1;
    udp_address[0] = (uint8_t) protocol;
    // ipv4
    if (protocol == PROTOCOL_UDP) {
        // 端口
        memcpy(udp_address + addrsz, &sa->v4.sin_port, sizeof(sa->v4.sin_port));
        addrsz += sizeof(sa->v4.sin_port);
        // ipv4的addr
        memcpy(udp_address + addrsz, &sa->v4.sin_addr, sizeof(sa->v4.sin_addr));
        addrsz += sizeof(sa->v4.sin_addr);
    } else {
        // ipv6
        // 端口
        memcpy(udp_address + addrsz, &sa->v6.sin6_port, sizeof(sa->v6.sin6_port));
        addrsz += sizeof(sa->v6.sin6_port);
        // ipv6的addr
        memcpy(udp_address + addrsz, &sa->v6.sin6_addr, sizeof(sa->v6.sin6_addr));
        addrsz += sizeof(sa->v6.sin6_addr);
    }
    return addrsz;
}

static int
forward_message_udp(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
    union sockaddr_all sa;
    socklen_t slen = sizeof(sa);
    int n = recvfrom(s->fd, ss->udpbuffer, MAX_UDP_PACKAGE, 0, &sa.s, &slen);
    if (n < 0) {
        switch (errno) {
            // 被中断,下次再读
            case EINTR:
            case AGAIN_WOULDBLOCK:
                return -1;
        }
        int error = errno;
        // close when error
        force_close(ss, s, l, result);
        result->data = strerror(error);
        return SOCKET_ERR;
    }
    // 统计
    stat_read(ss, s, n);

    uint8_t *data;
    if (slen == sizeof(sa.v4)) {
        if (s->protocol != PROTOCOL_UDP)
            return -1;
        // 最后填充udp地址
        // udpIP类型(ipv6) 1位,端口2位,ip地址4位
        data = MALLOC(n + 1 + 2 + 4);
        gen_udp_address(PROTOCOL_UDP, &sa, data + n);
    } else {
        if (s->protocol != PROTOCOL_UDPv6)
            return -1;
        // 最后填充udp地址
        // udpIP类型(ipv6) 1位,端口2位,ip地址16位
        data = MALLOC(n + 1 + 2 + 16);
        gen_udp_address(PROTOCOL_UDPv6, &sa, data + n);
    }
    // 复制接收到的数据到data
    memcpy(data, ss->udpbuffer, n);

    result->opaque = s->opaque;
    result->id = s->id;
    result->ud = n;
    result->data = (char *) data;

    return SOCKET_UDP;
}

static int
report_connect(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
    int error;
    socklen_t len = sizeof(error);
    int code = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &len);
    // 能拿到设置证明连接成功了？
    if (code < 0 || error) {
        error = code < 0 ? errno : error;
        force_close(ss, s, l, result);
        result->data = strerror(error);
        return SOCKET_ERR;
    } else {
        ATOM_STORE(&s->type, SOCKET_TYPE_CONNECTED);
        result->opaque = s->opaque;
        result->id = s->id;
        result->ud = 0;
        // 没有更多的发送数据
        if (nomore_sending_data(s)) {
            // 关闭写监听
            if (enable_write(ss, s, false)) {
                // 关闭写监听失败
                force_close(ss, s, l, result);
                result->data = "disable write failed";
                return SOCKET_ERR;
            }
        }
        union sockaddr_all u;
        socklen_t slen = sizeof(u);
        //  对于流套接字，一旦执行了connect(2) ，connect(2)的调用者可以使用getpeername()来获取peer的地址套接字(来获取对等体先前为套接字设置的地址)
        // getpeername () 返回连接到的对等体的地址 -> u
        //  如果成功，则返回零。如果出错，则返回 -1，并设置errno以指示错误。
        if (getpeername(s->fd, &u.s, &slen) == 0) {
            // sin_addr 是套接字中的 IP 地址（套接字结构还包含其他数据，例如端口)
            // sin_addr 的类型是 union，因此可以通过三种不同的方式访问它：s_un_b（四个 1 字节整数）、s_un_w（两个 2 字节整数）或 s_addr（一个 4 字节整数）
            void *sin_addr = (u.s.sa_family == AF_INET) ? (void *) &u.v4.sin_addr : (void *) &u.v6.sin6_addr;
            // 将 IPv4 和 IPv6 地址从二进制转换为文本形式,结果存在ss->buffer
            if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
                result->data = ss->buffer;
                return SOCKET_OPEN;
            }
        }
        result->data = NULL;
        return SOCKET_OPEN;
    }
}

static int
getname(union sockaddr_all *u, char *buffer, size_t sz) {
    char tmp[INET6_ADDRSTRLEN];
    void *sin_addr = (u->s.sa_family == AF_INET) ? (void *) &u->v4.sin_addr : (void *) &u->v6.sin6_addr;
    // 将 IPv4 和 IPv6 地址从二进制转换为文本形式,结果存在ss->buffer
    if (inet_ntop(u->s.sa_family, sin_addr, tmp, sizeof(tmp))) {
        int sin_port = ntohs((u->s.sa_family == AF_INET) ? u->v4.sin_port : u->v6.sin6_port);
        snprintf(buffer, sz, "%s:%d", tmp, sin_port);
        return 1;
    } else {
        buffer[0] = '\0';
        return 0;
    }
}

// return 0 when failed, or -1 when file limit
// 失败时返回 0，文件限制时返回 -1
static int
report_accept(struct socket_server *ss, struct socket *s, struct socket_message *result) {
    union sockaddr_all u;
    socklen_t len = sizeof(u);
    // 接收连接
    int client_fd = accept(s->fd, &u.s, &len);
    if (client_fd < 0) {
        // 当进程用尽文件描述符时，accept() 将失败并设置errno为 EMFILE
        // 如果达到了文件描述符的最大数量，则无法接受客户端连接。这可能是进程限制（errno EMFILE）或全局系统限制（errno ENFILE
        if (errno == EMFILE || errno == ENFILE) {
            result->opaque = s->opaque;
            result->id = s->id;
            result->ud = 0;
            result->data = strerror(errno);
            // 但是，本来应该被接受的底层连接并未关闭，因此似乎没有办法通知客户端应用程序代码无法处理该连接。
            // See https://stackoverflow.com/questions/47179793/how-to-gracefully-handle-accept-giving-emfile-and-close-the-connection
            // 如果文件描述符用进,用备用描述符来接收新连接,并且立马关掉
            if (ss->reserve_fd >= 0) {
                close(ss->reserve_fd);
                client_fd = accept(s->fd, &u.s, &len);
                if (client_fd >= 0) {
                    close(client_fd);
                }
                // 一个进程在此存在期间，会有一些文件被打开，从而会返回一些文件描述符，从shell中运行一个进程，默认会有3个文件描述符存在(0、１、2)，
                // 文件描述符0、１、2与标准输入、标准输出、标准错误输出相关联，这只是shell以及很多应用程序的惯例，而与内核无关
                // 0与进程的标准输入相关联，
                // １与进程的标准输出相关联，
                // 2与进程的标准错误输出相关联，
                // 一个进程当前有哪些打开的文件描述符可以通过/proc/进程ID/fd目录查看。　
                // 文件表中包含：文件状态标志、当前文件偏移量、v节点指针，每个打开的文件描述符(fd标志)在进程表中都有自己的文件表项，由文件指针指向。
                // dup() 复制一个现存的文件描述符,当调用dup函数时，
                // 内核在进程中创建一个新的文件描述符，此描述符是当前可用文件描述符的最小数值，这个文件描述符指向oldfd所拥有的文件表项
                ss->reserve_fd = dup(1);
            }
            return -1;
        } else {
            return 0;
        }
    }
    // 预留id
    int id = reserve_id(ss);
    if (id < 0) {
        close(client_fd);
        return 0;
    }
    socket_keepalive(client_fd);
    sp_nonblocking(client_fd);
    struct socket *ns = new_fd(ss, id, client_fd, PROTOCOL_TCP, s->opaque, false);
    if (ns == NULL) {
        close(client_fd);
        return 0;
    }
    // accept new one connection
    stat_read(ss, s, 1);

    ATOM_STORE(&ns->type, SOCKET_TYPE_PACCEPT);
    result->opaque = s->opaque;
    result->id = s->id;
    result->ud = id;
    result->data = NULL;

    if (getname(&u, ss->buffer, sizeof(ss->buffer))) {
        result->data = ss->buffer;
    }

    return 1;
}

static inline void
clear_closed_event(struct socket_server *ss, struct socket_message *result, int type) {
    if (type == SOCKET_CLOSE || type == SOCKET_ERR) {
        int id = result->id;
        int i;
        for (i = ss->event_index; i < ss->event_n; i++) {
            struct event *e = &ss->ev[i];
            struct socket *s = e->s;
            if (s) {
                //socket 是无效状态
                if (socket_invalid(s, id) && s->id == id) {
                    e->s = NULL;
                    break;
                }
            }
        }
    }
}

// return type
/*
 * 每当 epoll_wait 返回时，新的一轮网络事件的处理就会开始，指令的检查标记 checkctrl 也会被设为 1，来开启指令检查。
 * 每轮只会处理一次指令，会一直连续处理指令直到全部处理完。
 * 通过 has_cmd 来检查管道中有没有还没处理的命令数据,这一步是使用系统调用 select 来实现的，使用 select 来检查 recvctrl_fd 是否可读。
 * 虽说 recvctrl_fd 被加到了 epoll 中，　但是 epoll_wait 唤醒以后，如果是指令数据唤醒的，不会原地处理，而是等下一个循环处理，
 * 所以这里还要再检查一次是否可读，并没有以来 epoll 做标记之类的，　可能是为了处理简单一些。
 * 如果 recvctrl_fd 中有指令等待读取，则调用 ctrl_cmd 读取并执行指令。
 * 其中用了两次 block_readpipe 来读取管道中的数据，第一次读取了数据头，包括了命令类型和数据长度，第二次用第一次读取到的数据长度读取了数据。
 * block_readpipe 是用来从管道中读取数据的函数，可以看到，read 的返回值只处理了小于 0 的情况，并没有处理 n > 0 && n < size 的情况。
 * 这是因为对管道套接字执行 read 操作是一个原子操作，不会被别的情况比如信号之类的打断，所以只有两种可能，错误和全部读取完成。
 * 读取到了命令以后，根据命令类型，把数据交给不同的处理函数来处理即可。
 */
int
socket_server_poll(struct socket_server *ss, struct socket_message *result, int *more) {
    for (;;) {
        // 开启了指令检查
        if (ss->checkctrl) {
            // 这里去做SELECT,拿到发生的事件指令
            if (has_cmd(ss)) {
                // 处理对应的指令,返回结果
                int type = ctrl_cmd(ss, result);
                // 非正常返回
                if (type != -1) {
                    // 清除已经无效化socket的相关事件
                    // type == SOCKET_CLOSE || type == SOCKET_ERR
                    // 尝试去做清除,只有无效化后的socket会被清除
                    clear_closed_event(ss, result, type);
                    return type;
                } else
                    // ctrl_cmd正常返回,结束这次循环,下次继续处理指令
                    continue;
            } else {
                // 没有指令需要处理时,关闭指令检查,下次循环就直接跳过指令处理部分
                ss->checkctrl = 0;
            }
        }
        // 指令处理完毕,开始处理事件

        // 如果当前收集到的事件集合已经处理完毕
        if (ss->event_index == ss->event_n) {
            // kevent 去获得最新的事件集合
            ss->event_n = sp_wait(ss->event_fd, ss->ev, MAX_EVENT);
            // 下次循环需要检查控制指令
            ss->checkctrl = 1;
            // 多次处理参数置0
            if (more) {
                *more = 0;
            }
            // 重置开始处理的event下标
            ss->event_index = 0;
            // 没有事件发生
            if (ss->event_n <= 0) {
                // 置零可以通过ss->event_index == ss->event_n
                ss->event_n = 0;
                int err = errno;
                if (err != EINTR) {
                    skynet_error(NULL, "socket-server: %s", strerror(err));
                }
                // 没有事件也跳出循环
                continue;
            }
        }
        // 开始处理事件
        struct event *e = &ss->ev[ss->event_index++];
        struct socket *s = e->s;
        // 事件关联socket 已经关闭
        if (s == NULL) {
            // dispatch pipe message at beginning
            // 在开头发送管道消息
            continue;
        }
        // 这个计数统计结构体用来做一个线程的可重入锁,结构体用的是栈内存,线程独享。
        // 只要当前线程加锁(结构体内部真正的自旋锁)成功,后续当前线程进入就不需要再次加锁。
        struct socket_lock l;
        socket_lock_init(s, &l);
        // 连接状态
        switch (ATOM_LOAD(&s->type)) {
            // socket还在连接中,未连接成功
            case SOCKET_TYPE_CONNECTING:
                // 检测连接状态
                return report_connect(ss, s, &l, result);
                // 监听socket,去accept
            case SOCKET_TYPE_LISTEN: {
                int ok = report_accept(ss, s, result);
                if (ok > 0) {
                    return SOCKET_ACCEPT;
                }
                if (ok < 0) {
                    return SOCKET_ERR;
                }
                // when ok == 0, retry
                // 连接失败
                break;
            }
                // 套接字无效
            case SOCKET_TYPE_INVALID:
                skynet_error(NULL, "socket-server: invalid socket");
                break;
            default:
                // 其它状态就走正常的处理
                // 如果是可读事件
                if (e->read) {
                    int type;
                    // tcp 协议
                    if (s->protocol == PROTOCOL_TCP) {
                        // 转发协议,最终读到的数据在result中
                        type = forward_message_tcp(ss, s, &l, result);
                        // MORE可以多次处理,数据没读完,这个事件保留
                        if (type == SOCKET_MORE) {
                            --ss->event_index;
                            return SOCKET_DATA;
                        }
                    } else {
                        // UDP 处理,最终读到的数据在result中
                        type = forward_message_udp(ss, s, &l, result);
                        if (type == SOCKET_UDP) {
                            // try read again
                            --ss->event_index;
                            return SOCKET_UDP;
                        }
                    }
                    // 可读 & 可写
                    if (e->write && type != SOCKET_CLOSE && type != SOCKET_ERR) {
                        // Try to dispatch write message next step if write flag set.
                        e->read = false;
                        // 可读处理完了,回退下次处理这个事件的可写部分
                        --ss->event_index;
                    }
                    if (type == -1)
                        break;
                    return type;
                }
                // 可写事件
                if (e->write) {
                    int type = send_buffer(ss, s, &l, result);
                    if (type == -1)
                        break;
                    return type;
                }
                // 错误
                if (e->error) {
                    //  getsockopt、setsockopt - 获取和设置套接字选项
                    //  int getsockopt(int sockfd , int level , int optname ,
                    //                      void optval [restrict *. optlen ],
                    //                      socklen_t *restrict optlen );
                    //  int setsockopt(int sockfd , int level , int optname ,
                    //                      const void optval [. optlen ],
                    //                      socklen_t optlen );
                    // 成功时，标准选项将返回零。出错时，将返回 -1，并设置errno以指示错误。
                    // 读取error
                    int error;
                    socklen_t len = sizeof(error);
                    int code = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &len);
                    const char *err = NULL;
                    // 读取error出错
                    if (code < 0) {
                        err = strerror(errno);
                    } else if (error != 0) {
                        // 读取error成功,且存在error
                        err = strerror(error);
                    } else {
                        err = "Unknown error";
                    }
                    return report_error(s, result, err);
                }
                // 关于shutdown:
                // 执行shutdown(SHUT_WR)操作时会发送一个FIN,并用SEND_SHUTDOWN标记套接字
                // 执行shutdown(SHUT_RD)时不发送任何内容,并用RCV_SHUTDOWN标记套接字
                // 接收FIN用,RCV_SHUTDOWN标记套接字
                //
                // 关于epoll:
                // 如果套接字标记有SEND_SHUTDOWN和RCV_SHUTDOWN,poll将返回EPOLLHUP。EPOLLHUP 表示读写都关闭
                // 如果套接字标记有RCV_SHUTDOWN,poll将返回EPOLLRDHUP。
                // EPOLLRDHUP 可以作为一种读关闭的标志,不能读的意思内核不能再往内核缓冲区中增加新的内容。已经在内核缓冲区中的内容，用户态依然能够读取到
                //
                // 因此该HUP事件可以理解为:
                // EPOLLRDHUP:已收到FIN或已调用shutdown(SHUT_RD)。无论如何读取套接字已半挂起，也就是说，您将不会再读取任何数据。
                // EPOLLHUP:您已挂起两个半套接字。读取半套接字与上一点类似，对于发送半套接字,执行的操作类似于shutdown(SHUT_WR)。
                //
                // 为了完成正常关闭,我会这样做:
                // 执行shutdown(SHUT_WR)发送FIN并标记发送数据的结束
                // 通过轮询等待对等方执行相同操作，直到获得EPOLLRDHUP。
                // 现在您可以优雅地关闭套接字了。
                if (e->eof) {
                    // EOF 指对端关闭(FIN)
                    // For epoll (at least), FIN packets are exchanged both ways.
                    // 对于 epoll（至少）来说，FIN 数据包是双向交换的
                    // See: https://stackoverflow.com/questions/52976152/tcp-when-is-epollhup-generated
                    int halfclose = halfclose_read(s);
                    // 对方关闭,这边也关闭
                    force_close(ss, s, &l, result);
                    // 不是半关闭,返回关闭状态给上层
                    if (!halfclose) {
                        return SOCKET_CLOSE;
                    }
                }
                break;
        }
    }
}

static void
send_request(struct socket_server *ss, struct request_package *request, char type, int len) {
    request->header[6] = (uint8_t) type;
    request->header[7] = (uint8_t) len;
    const char *req = (const char *) request + offsetof(struct request_package, header[6]);
    for (;;) {
        ssize_t n = write(ss->sendctrl_fd, req, len + 2);
        if (n < 0) {
            if (errno != EINTR) {
                skynet_error(NULL, "socket-server : send ctrl command error %s.", strerror(errno));
            }
            continue;
        }
        assert(n == len + 2);
        return;
    }
}

static int
open_request(struct socket_server *ss, struct request_package *req, uintptr_t opaque, const char *addr, int port) {
    int len = strlen(addr);
    if (len + sizeof(req->u.open) >= 256) {
        skynet_error(NULL, "socket-server : Invalid addr %s.", addr);
        return -1;
    }
    int id = reserve_id(ss);
    if (id < 0)
        return -1;
    req->u.open.opaque = opaque;
    req->u.open.id = id;
    req->u.open.port = port;
    memcpy(req->u.open.host, addr, len);
    req->u.open.host[len] = '\0';

    return len;
}

int
socket_server_connect(struct socket_server *ss, uintptr_t opaque, const char *addr, int port) {
    struct request_package request;
    int len = open_request(ss, &request, opaque, addr, port);
    if (len < 0)
        return -1;
    send_request(ss, &request, 'O', sizeof(request.u.open) + len);
    return request.u.open.id;
}

static inline int
can_direct_write(struct socket *s, int id) {
    return s->id == id && nomore_sending_data(s) && ATOM_LOAD(&s->type) == SOCKET_TYPE_CONNECTED &&
           ATOM_LOAD(&s->udpconnecting) == 0;
}

// return -1 when error, 0 when success
int
socket_server_send(struct socket_server *ss, struct socket_sendbuffer *buf) {
    int id = buf->id;
    struct socket *s = &ss->slot[HASH_ID(id)];
    if (socket_invalid(s, id) || s->closing) {
        free_buffer(ss, buf);
        return -1;
    }

    struct socket_lock l;
    socket_lock_init(s, &l);

    if (can_direct_write(s, id) && socket_trylock(&l)) {
        // may be we can send directly, double check
        if (can_direct_write(s, id)) {
            // send directly
            struct send_object so;
            send_object_init_from_sendbuffer(ss, &so, buf);
            ssize_t n;
            if (s->protocol == PROTOCOL_TCP) {
                n = write(s->fd, so.buffer, so.sz);
            } else {
                union sockaddr_all sa;
                socklen_t sasz = udp_socket_address(s, s->p.udp_address, &sa);
                if (sasz == 0) {
                    skynet_error(NULL, "socket-server : set udp (%d) address first.", id);
                    socket_unlock(&l);
                    so.free_func((void *) buf->buffer);
                    return -1;
                }
                n = sendto(s->fd, so.buffer, so.sz, 0, &sa.s, sasz);
            }
            if (n < 0) {
                // ignore error, let socket thread try again
                n = 0;
            }
            stat_write(ss, s, n);
            if (n == so.sz) {
                // write done
                socket_unlock(&l);
                so.free_func((void *) buf->buffer);
                return 0;
            }
            // write failed, put buffer into s->dw_* , and let socket thread send it. see send_buffer()
            s->dw_buffer = clone_buffer(buf, &s->dw_size);
            s->dw_offset = n;

            socket_unlock(&l);

            struct request_package request;
            request.u.send.id = id;
            request.u.send.sz = 0;
            request.u.send.buffer = NULL;

            // let socket thread enable write event
            send_request(ss, &request, 'W', sizeof(request.u.send));

            return 0;
        }
        socket_unlock(&l);
    }

    inc_sending_ref(s, id);

    struct request_package request;
    request.u.send.id = id;
    request.u.send.buffer = clone_buffer(buf, &request.u.send.sz);

    send_request(ss, &request, 'D', sizeof(request.u.send));
    return 0;
}

// return -1 when error, 0 when success
int
socket_server_send_lowpriority(struct socket_server *ss, struct socket_sendbuffer *buf) {
    int id = buf->id;

    struct socket *s = &ss->slot[HASH_ID(id)];
    if (socket_invalid(s, id)) {
        free_buffer(ss, buf);
        return -1;
    }

    inc_sending_ref(s, id);

    struct request_package request;
    request.u.send.id = id;
    request.u.send.buffer = clone_buffer(buf, &request.u.send.sz);

    send_request(ss, &request, 'P', sizeof(request.u.send));
    return 0;
}

void
socket_server_exit(struct socket_server *ss) {
    struct request_package request;
    send_request(ss, &request, 'X', 0);
}

void
socket_server_close(struct socket_server *ss, uintptr_t opaque, int id) {
    struct request_package request;
    request.u.close.id = id;
    request.u.close.shutdown = 0;
    request.u.close.opaque = opaque;
    send_request(ss, &request, 'K', sizeof(request.u.close));
}


void
socket_server_shutdown(struct socket_server *ss, uintptr_t opaque, int id) {
    struct request_package request;
    request.u.close.id = id;
    request.u.close.shutdown = 1;
    request.u.close.opaque = opaque;
    send_request(ss, &request, 'K', sizeof(request.u.close));
}

// return -1 means failed
// or return AF_INET or AF_INET6
static int
do_bind(const char *host, int port, int protocol, int *family) {
    int fd;
    int status;
    int reuse = 1;
    struct addrinfo ai_hints;
    struct addrinfo *ai_list = NULL;
    char portstr[16];
    if (host == NULL || host[0] == 0) {
        host = "0.0.0.0";    // INADDR_ANY
    }
    sprintf(portstr, "%d", port);
    memset(&ai_hints, 0, sizeof(ai_hints));
    ai_hints.ai_family = AF_UNSPEC;
    if (protocol == IPPROTO_TCP) {
        ai_hints.ai_socktype = SOCK_STREAM;
    } else {
        assert(protocol == IPPROTO_UDP);
        ai_hints.ai_socktype = SOCK_DGRAM;
    }
    ai_hints.ai_protocol = protocol;

    status = getaddrinfo(host, portstr, &ai_hints, &ai_list);
    if (status != 0) {
        return -1;
    }
    *family = ai_list->ai_family;
    fd = socket(*family, ai_list->ai_socktype, 0);
    if (fd < 0) {
        goto _failed_fd;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *) &reuse, sizeof(int)) == -1) {
        goto _failed;
    }
    status = bind(fd, (struct sockaddr *) ai_list->ai_addr, ai_list->ai_addrlen);
    if (status != 0)
        goto _failed;

    freeaddrinfo(ai_list);
    return fd;
    _failed:
    close(fd);
    _failed_fd:
    freeaddrinfo(ai_list);
    return -1;
}

static int
do_listen(const char *host, int port, int backlog) {
    int family = 0;
    int listen_fd = do_bind(host, port, IPPROTO_TCP, &family);
    if (listen_fd < 0) {
        return -1;
    }
    if (listen(listen_fd, backlog) == -1) {
        close(listen_fd);
        return -1;
    }
    return listen_fd;
}

int
socket_server_listen(struct socket_server *ss, uintptr_t opaque, const char *addr, int port, int backlog) {
    int fd = do_listen(addr, port, backlog);
    if (fd < 0) {
        return -1;
    }
    struct request_package request;
    int id = reserve_id(ss);
    if (id < 0) {
        close(fd);
        return id;
    }
    request.u.listen.opaque = opaque;
    request.u.listen.id = id;
    request.u.listen.fd = fd;
    send_request(ss, &request, 'L', sizeof(request.u.listen));
    return id;
}

int
socket_server_bind(struct socket_server *ss, uintptr_t opaque, int fd) {
    struct request_package request;
    int id = reserve_id(ss);
    if (id < 0)
        return -1;
    request.u.bind.opaque = opaque;
    request.u.bind.id = id;
    request.u.bind.fd = fd;
    send_request(ss, &request, 'B', sizeof(request.u.bind));
    return id;
}

void
socket_server_start(struct socket_server *ss, uintptr_t opaque, int id) {
    struct request_package request;
    request.u.resumepause.id = id;
    request.u.resumepause.opaque = opaque;
    send_request(ss, &request, 'R', sizeof(request.u.resumepause));
}

void
socket_server_pause(struct socket_server *ss, uintptr_t opaque, int id) {
    struct request_package request;
    request.u.resumepause.id = id;
    request.u.resumepause.opaque = opaque;
    send_request(ss, &request, 'S', sizeof(request.u.resumepause));
}

void
socket_server_nodelay(struct socket_server *ss, int id) {
    struct request_package request;
    request.u.setopt.id = id;
    request.u.setopt.what = TCP_NODELAY;
    request.u.setopt.value = 1;
    send_request(ss, &request, 'T', sizeof(request.u.setopt));
}

void
socket_server_userobject(struct socket_server *ss, struct socket_object_interface *soi) {
    ss->soi = *soi;
}

// UDP

int
socket_server_udp(struct socket_server *ss, uintptr_t opaque, const char *addr, int port) {
    int fd;
    int family;
    if (port != 0 || addr != NULL) {
        // bind
        fd = do_bind(addr, port, IPPROTO_UDP, &family);
        if (fd < 0) {
            return -1;
        }
    } else {
        family = AF_INET;
        fd = socket(family, SOCK_DGRAM, 0);
        if (fd < 0) {
            return -1;
        }
    }
    sp_nonblocking(fd);

    int id = reserve_id(ss);
    if (id < 0) {
        close(fd);
        return -1;
    }
    struct request_package request;
    request.u.udp.id = id;
    request.u.udp.fd = fd;
    request.u.udp.opaque = opaque;
    request.u.udp.family = family;

    send_request(ss, &request, 'U', sizeof(request.u.udp));
    return id;
}

int
socket_server_udp_send(struct socket_server *ss, const struct socket_udp_address *addr, struct socket_sendbuffer *buf) {
    int id = buf->id;
    struct socket *s = &ss->slot[HASH_ID(id)];
    if (socket_invalid(s, id)) {
        free_buffer(ss, buf);
        return -1;
    }

    const uint8_t *udp_address = (const uint8_t *) addr;
    int addrsz;
    switch (udp_address[0]) {
        case PROTOCOL_UDP:
            addrsz = 1 + 2 + 4;        // 1 type, 2 port, 4 ipv4
            break;
        case PROTOCOL_UDPv6:
            addrsz = 1 + 2 + 16;    // 1 type, 2 port, 16 ipv6
            break;
        default:
            free_buffer(ss, buf);
            return -1;
    }

    struct socket_lock l;
    socket_lock_init(s, &l);

    if (can_direct_write(s, id) && socket_trylock(&l)) {
        // may be we can send directly, double check
        if (can_direct_write(s, id)) {
            // send directly
            struct send_object so;
            send_object_init_from_sendbuffer(ss, &so, buf);
            union sockaddr_all sa;
            socklen_t sasz = udp_socket_address(s, udp_address, &sa);
            if (sasz == 0) {
                socket_unlock(&l);
                so.free_func((void *) buf->buffer);
                return -1;
            }
            int n = sendto(s->fd, so.buffer, so.sz, 0, &sa.s, sasz);
            if (n >= 0) {
                // sendto succ
                stat_write(ss, s, n);
                socket_unlock(&l);
                so.free_func((void *) buf->buffer);
                return 0;
            }
        }
        socket_unlock(&l);
        // let socket thread try again, udp doesn't care the order
    }

    struct request_package request;
    request.u.send_udp.send.id = id;
    request.u.send_udp.send.buffer = clone_buffer(buf, &request.u.send_udp.send.sz);

    memcpy(request.u.send_udp.address, udp_address, addrsz);

    send_request(ss, &request, 'A', sizeof(request.u.send_udp.send) + addrsz);
    return 0;
}

int
socket_server_udp_connect(struct socket_server *ss, int id, const char *addr, int port) {
    struct socket *s = &ss->slot[HASH_ID(id)];
    if (socket_invalid(s, id)) {
        return -1;
    }
    struct socket_lock l;
    socket_lock_init(s, &l);
    socket_lock(&l);
    if (socket_invalid(s, id)) {
        socket_unlock(&l);
        return -1;
    }
    ATOM_FINC(&s->udpconnecting);
    socket_unlock(&l);

    int status;
    struct addrinfo ai_hints;
    struct addrinfo *ai_list = NULL;
    char portstr[16];
    sprintf(portstr, "%d", port);
    memset(&ai_hints, 0, sizeof(ai_hints));
    ai_hints.ai_family = AF_UNSPEC;
    ai_hints.ai_socktype = SOCK_DGRAM;
    ai_hints.ai_protocol = IPPROTO_UDP;

    status = getaddrinfo(addr, portstr, &ai_hints, &ai_list);
    if (status != 0) {
        return -1;
    }
    struct request_package request;
    request.u.set_udp.id = id;
    int protocol;

    if (ai_list->ai_family == AF_INET) {
        protocol = PROTOCOL_UDP;
    } else if (ai_list->ai_family == AF_INET6) {
        protocol = PROTOCOL_UDPv6;
    } else {
        freeaddrinfo(ai_list);
        return -1;
    }

    int addrsz = gen_udp_address(protocol, (union sockaddr_all *) ai_list->ai_addr, request.u.set_udp.address);

    freeaddrinfo(ai_list);

    send_request(ss, &request, 'C', sizeof(request.u.set_udp) - sizeof(request.u.set_udp.address) + addrsz);

    return 0;
}

const struct socket_udp_address *
socket_server_udp_address(struct socket_server *ss, struct socket_message *msg, int *addrsz) {
    uint8_t *address = (uint8_t *) (msg->data + msg->ud);
    int type = address[0];
    switch (type) {
        case PROTOCOL_UDP:
            *addrsz = 1 + 2 + 4;
            break;
        case PROTOCOL_UDPv6:
            *addrsz = 1 + 2 + 16;
            break;
        default:
            return NULL;
    }
    return (const struct socket_udp_address *) address;
}


struct socket_info *
socket_info_create(struct socket_info *last) {
    struct socket_info *si = skynet_malloc(sizeof(*si));
    memset(si, 0, sizeof(*si));
    si->next = last;
    return si;
}

void
socket_info_release(struct socket_info *si) {
    while (si) {
        struct socket_info *temp = si;
        si = si->next;
        skynet_free(temp);
    }
}

static int
query_info(struct socket *s, struct socket_info *si) {
    union sockaddr_all u;
    socklen_t slen = sizeof(u);
    int closing = 0;
    switch (ATOM_LOAD(&s->type)) {
        case SOCKET_TYPE_BIND:
            si->type = SOCKET_INFO_BIND;
            si->name[0] = '\0';
            break;
        case SOCKET_TYPE_LISTEN:
            si->type = SOCKET_INFO_LISTEN;
            if (getsockname(s->fd, &u.s, &slen) == 0) {
                getname(&u, si->name, sizeof(si->name));
            }
            break;
        case SOCKET_TYPE_HALFCLOSE_READ:
        case SOCKET_TYPE_HALFCLOSE_WRITE:
            closing = 1;
        case SOCKET_TYPE_CONNECTED:
            if (s->protocol == PROTOCOL_TCP) {
                si->type = closing ? SOCKET_INFO_CLOSING : SOCKET_INFO_TCP;
                if (getpeername(s->fd, &u.s, &slen) == 0) {
                    getname(&u, si->name, sizeof(si->name));
                }
            } else {
                si->type = SOCKET_INFO_UDP;
                if (udp_socket_address(s, s->p.udp_address, &u)) {
                    getname(&u, si->name, sizeof(si->name));
                }
            }
            break;
        default:
            return 0;
    }
    si->id = s->id;
    si->opaque = (uint64_t) s->opaque;
    si->read = s->stat.read;
    si->write = s->stat.write;
    si->rtime = s->stat.rtime;
    si->wtime = s->stat.wtime;
    si->wbuffer = s->wb_size;
    si->reading = s->reading;
    si->writing = s->writing;

    return 1;
}

struct socket_info *
socket_server_info(struct socket_server *ss) {
    int i;
    struct socket_info *si = NULL;
    for (i = 0; i < MAX_SOCKET; i++) {
        struct socket *s = &ss->slot[i];
        int id = s->id;
        struct socket_info temp;
        if (query_info(s, &temp) && s->id == id) {
            // socket_server_info may call in different thread, so check socket id again
            si = socket_info_create(si);
            temp.next = si->next;
            *si = temp;
        }
    }
    return si;
}
