/*

  Copyright (c) 2017 Martin Sustrik

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"),
  to deal in the Software without restriction, including without limitation
  the rights to use, copy, modify, merge, publish, distribute, sublicense,
  and/or sell copies of the Software, and to permit persons to whom
  the Software is furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
  IN THE SOFTWARE.

*/

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>

#define DILL_DISABLE_RAW_NAMES
#include "libdillimpl.h"
#include "fd.h"
#include "utils.h"

static int dill_ipc_resolve(const char *addr, struct sockaddr_un *su);
static int dill_ipc_makeconn(int fd, void *mem);

dill_unique_id(dill_ipc_listener_type);
dill_unique_id(dill_ipc_type);

/******************************************************************************/
/*  UNIX connection socket                                                    */
/******************************************************************************/

static void *dill_ipc_hquery(struct dill_hvfs *hvfs, const void *type);
static void dill_ipc_hclose(struct dill_hvfs *hvfs);
static int dill_ipc_bsendl(struct dill_bsock_vfs *bvfs,
    struct dill_iolist *first, struct dill_iolist *last, int64_t deadline);
static int dill_ipc_brecvl(struct dill_bsock_vfs *bvfs,
    struct dill_iolist *first, struct dill_iolist *last, int64_t deadline);

struct dill_ipc_conn {
    struct dill_hvfs hvfs;
    struct dill_bsock_vfs bvfs;
    int fd;
    struct dill_fd_rxbuf rxbuf;
    unsigned int busy : 1;
    unsigned int indone : 1;
    unsigned int outdone : 1;
    unsigned int inerr : 1;
    unsigned int outerr : 1;
    unsigned int mem : 1;
};

DILL_CHECK_STORAGE(dill_ipc_conn, dill_ipc_storage)

static void *dill_ipc_hquery(struct dill_hvfs *hvfs, const void *type) {
    struct dill_ipc_conn *self = (struct dill_ipc_conn*)hvfs;
    if(type == dill_bsock_type) return &self->bvfs;
    if(type == dill_ipc_type) return self;
    errno = ENOTSUP;
    return NULL;
}

int dill_ipc_connect_mem(const char *addr, struct dill_ipc_storage *mem,
      int64_t deadline) {
    int err;
    if(dill_slow(!mem)) {err = EINVAL; goto error1;}
    /* Create a UNIX address out of the address string. */
    struct sockaddr_un su;
    int rc = dill_ipc_resolve(addr, &su);
    if(rc < 0) {err = errno; goto error1;}
    /* Open a socket. */
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if(dill_slow(s < 0)) {err = errno; goto error1;}
    /* Set it to non-blocking mode. */
    rc = dill_fd_unblock(s);
    if(dill_slow(rc < 0)) {err = errno; goto error2;}
    /* Connect to the remote endpoint. */
    rc = dill_fd_connect(s, (struct sockaddr*)&su, sizeof(su), deadline);
    if(dill_slow(rc < 0)) {err = errno; goto error2;}
    /* Create the handle. */
    int h = dill_ipc_makeconn(s, mem);
    if(dill_slow(h < 0)) {err = errno; goto error2;}
    return h;
error2:
    dill_fd_close(s);
error1:
    errno = err;
    return -1;
}

int dill_ipc_connect(const char *addr, int64_t deadline) {
    int err;
    struct dill_ipc_conn *obj = malloc(sizeof(struct dill_ipc_conn));
    if(dill_slow(!obj)) {err = ENOMEM; goto error1;}
    int s = dill_ipc_connect_mem(addr, (struct dill_ipc_storage*)obj, deadline);
    if(dill_slow(s < 0)) {err = errno; goto error2;}
    obj->mem = 0;
    return s;
error2:
    free(obj);
error1:
    errno = err;
    return -1;
}

static int dill_ipc_bsendl(struct dill_bsock_vfs *bvfs,
      struct dill_iolist *first, struct dill_iolist *last, int64_t deadline) {
    struct dill_ipc_conn *self = dill_cont(bvfs, struct dill_ipc_conn, bvfs);
    if(dill_slow(self->busy)) {errno = EBUSY; return -1;}
    if(dill_slow(self->outdone)) {errno = EPIPE; return -1;}
    if(dill_slow(self->outerr)) {errno = ECONNRESET; return -1;}
    self->busy = 1;
    ssize_t sz = dill_fd_send(self->fd, first, last, deadline);
    self->busy = 0;
    if(dill_fast(sz >= 0)) return sz;
    self->outerr = 1;
    return -1;
}

static int dill_ipc_brecvl(struct dill_bsock_vfs *bvfs,
      struct dill_iolist *first, struct dill_iolist *last, int64_t deadline) {
    struct dill_ipc_conn *self = dill_cont(bvfs, struct dill_ipc_conn, bvfs);
    if(dill_slow(self->busy)) {errno = EBUSY; return -1;}
    if(dill_slow(self->indone)) {errno = EPIPE; return -1;}
    if(dill_slow(self->inerr)) {errno = ECONNRESET; return -1;}
    self->busy = 1;
    int rc = dill_fd_recv(self->fd, &self->rxbuf, first, last, deadline);
    self->busy = 0;
    if(dill_fast(rc == 0)) return 0;
    if(errno == EPIPE) self->indone = 1;
    else self->inerr = 1;
    return -1;
}

int dill_ipc_done(int s, int64_t deadline) {
    struct dill_ipc_conn *self = dill_hquery(s, dill_ipc_type);
    if(dill_slow(!self)) return -1;
    if(dill_slow(self->outdone)) {errno = EPIPE; return -1;}
    if(dill_slow(self->outerr)) {errno = ECONNRESET; return -1;}
    /* Shutdown is done asynchronously on kernel level.
       No need to use the deadline. */
    int rc = shutdown(self->fd, SHUT_WR);
    if(dill_slow(rc < 0)) {
        if(errno == ENOTCONN) {self->outerr = 1; errno = ECONNRESET; return -1;}
        if(errno == ENOBUFS) {self->outerr = 1; errno = ENOMEM; return -1;}
        dill_assert(0);
    }
    self->outdone = 1;
    return 0;
}

int dill_ipc_close(int s, int64_t deadline) {
    int err;
    /* Listener socket needs no special treatment. */
    if(dill_hquery(s, dill_ipc_listener_type)) {
        return dill_hclose(s);
    }
    struct dill_ipc_conn *self = dill_hquery(s, dill_ipc_type);
    if(dill_slow(!self)) return -1;
    if(dill_slow(self->inerr || self->outerr)) {err = ECONNRESET; goto error;}
    /* If not done already, flush the outbound data and start the terminal
       handshake. */
    if(!self->outdone) {
        int rc = dill_ipc_done(s, deadline);
        if(dill_slow(rc < 0)) {err = errno; goto error;}
    }
    /* Now we are going to read all the inbound data until we reach end of the
       stream. That way we can be sure that the peer either received all our
       data or consciously closed the connection without reading all of it. */
    int rc = dill_ipc_brecvl(&self->bvfs, NULL, NULL, deadline);
    dill_assert(rc < 0);
    if(dill_slow(errno != EPIPE)) {err = errno; goto error;}
    dill_ipc_hclose(&self->hvfs);
    return 0;
error:
    dill_ipc_hclose(&self->hvfs);
    errno = err;
    return -1;
}

static void dill_ipc_hclose(struct dill_hvfs *hvfs) {
    struct dill_ipc_conn *self = (struct dill_ipc_conn*)hvfs;
    dill_fd_close(self->fd);
    dill_fd_termrxbuf(&self->rxbuf);
    if(!self->mem) free(self);
}

/******************************************************************************/
/*  UNIX listener socket                                                      */
/******************************************************************************/

static void *dill_ipc_listener_hquery(struct dill_hvfs *hvfs, const void *type);
static void dill_ipc_listener_hclose(struct dill_hvfs *hvfs);

struct dill_ipc_listener {
    struct dill_hvfs hvfs;
    int fd;
    unsigned int mem : 1;
};

DILL_CHECK_STORAGE(dill_ipc_listener, dill_ipc_listener_storage)

static void *dill_ipc_listener_hquery(struct dill_hvfs *hvfs, const void *type) {
    struct dill_ipc_listener *self = (struct dill_ipc_listener*)hvfs;
    if(type == dill_ipc_listener_type) return self;
    errno = ENOTSUP;
    return NULL;
}

int dill_ipc_listen_mem(const char *addr, int backlog,
      struct dill_ipc_listener_storage *mem) {
    int err;
    /* Create a UNIX address out of the address string. */
    struct sockaddr_un su;
    int rc = dill_ipc_resolve(addr, &su);
    if(rc < 0) {err = errno; goto error1;}
    /* Open the listening socket. */
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if(dill_slow(s < 0)) {err = errno; goto error1;}
    /* Set it to non-blocking mode. */
    rc = dill_fd_unblock(s);
    if(dill_slow(rc < 0)) {err = errno; goto error2;}
    /* Start listening for incoming connections. */
    rc = bind(s, (struct sockaddr*)&su, sizeof(su));
    if(dill_slow(rc < 0)) {err = errno; goto error2;}
    rc = listen(s, backlog);
    if(dill_slow(rc < 0)) {err = errno; goto error2;}
    /* Create the object. */
    struct dill_ipc_listener *self = (struct dill_ipc_listener*)mem;
    self->hvfs.query = dill_ipc_listener_hquery;
    self->hvfs.close = dill_ipc_listener_hclose;
    self->fd = s;
    self->mem = 1;
    /* Create handle. */
    int h = dill_hmake(&self->hvfs);
    if(dill_slow(h < 0)) {err = errno; goto error2;}
    return h;
error2:
    close(s);
error1:
    errno = err;
    return -1;
}

int dill_ipc_listen(const char *addr, int backlog) {
    int err;
    struct dill_ipc_listener *obj = malloc(sizeof(struct dill_ipc_listener));
    if(dill_slow(!obj)) {err = ENOMEM; goto error1;}
    int ls = dill_ipc_listen_mem(addr, backlog,
        (struct dill_ipc_listener_storage*)obj);
    if(dill_slow(ls < 0)) {err = errno; goto error2;}
    obj->mem = 0;
    return ls;
error2:
    free(obj);
error1:
    errno = err;
    return -1;
}

int dill_ipc_accept_mem(int s, struct dill_ipc_storage *mem, int64_t deadline) {
    int err;
    if(dill_slow(!mem)) {err = EINVAL; goto error1;}
    /* Retrieve the listener object. */
    struct dill_ipc_listener *lst = dill_hquery(s, dill_ipc_listener_type);
    if(dill_slow(!lst)) {err = errno; goto error1;}
    /* Try to get new connection in a non-blocking way. */
    int as = dill_fd_accept(lst->fd, NULL, NULL, deadline);
    if(dill_slow(as < 0)) {err = errno; goto error1;}
    /* Set it to non-blocking mode. */
    int rc = dill_fd_unblock(as);
    if(dill_slow(rc < 0)) {err = errno; goto error2;}
    /* Create the handle. */
    int h = dill_ipc_makeconn(as, (struct dill_ipc_conn*)mem);
    if(dill_slow(h < 0)) {err = errno; goto error2;}
    return h;
error2:
    dill_fd_close(as);
error1:
    errno = err;
    return -1;
}

int dill_ipc_accept(int s, int64_t deadline) {
    int err;
    struct dill_ipc_conn *obj = malloc(sizeof(struct dill_ipc_conn));
    if(dill_slow(!obj)) {err = ENOMEM; goto error1;}
    int as = dill_ipc_accept_mem(s, (struct dill_ipc_storage*)obj, deadline);
    if(dill_slow(as < 0)) {err = errno; goto error2;}
    obj->mem = 0;
    return as;
error2:
    free(obj);
error1:
    errno = err;
    return -1;
}

static void dill_ipc_listener_hclose(struct dill_hvfs *hvfs) {
    struct dill_ipc_listener *self = (struct dill_ipc_listener*)hvfs;
    dill_fd_close(self->fd);
    if(!self->mem) free(self);
}

/******************************************************************************/
/*  UNIX pair                                                                 */
/******************************************************************************/

int dill_ipc_pair_mem(struct dill_ipc_pair_storage *mem, int s[2]) {
    int err;
    if(dill_slow(!mem)) {err = EINVAL; goto error1;}
    /* Create the pair. */
    int fds[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    if(rc < 0) {err = errno; goto error1;}
    /* Set the sockets to non-blocking mode. */
    rc = dill_fd_unblock(fds[0]);
    if(dill_slow(rc < 0)) {err = errno; goto error3;}
    rc = dill_fd_unblock(fds[1]);
    if(dill_slow(rc < 0)) {err = errno; goto error3;}
    /* Create the handles. */
    struct dill_ipc_conn *conns = (struct dill_ipc_conn*)mem;
    s[0] = dill_ipc_makeconn(fds[0], &conns[0]);
    if(dill_slow(s[0] < 0)) {err = errno; goto error3;}
    s[1] = dill_ipc_makeconn(fds[1], &conns[1]);
    if(dill_slow(s[1] < 0)) {err = errno; goto error4;}
    return 0;
error4:
    rc = dill_hclose(s[0]);
    goto error2;
error3:
    dill_fd_close(fds[0]);
error2:
    dill_fd_close(fds[1]);
error1:
    errno = err;
    return -1;
}

int dill_ipc_pair(int s[2]) {
    int err;
    /* Create the pair. */
    int fds[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    if(rc < 0) {err = errno; goto error1;}
    /* Set the sockets to non-blocking mode. */
    rc = dill_fd_unblock(fds[0]);
    if(dill_slow(rc < 0)) {err = errno; goto error3;}
    rc = dill_fd_unblock(fds[1]);
    if(dill_slow(rc < 0)) {err = errno; goto error3;}
    /* Allocate the memory. */
    struct dill_ipc_conn *conn0 = malloc(sizeof(struct dill_ipc_conn));
    if(dill_slow(!conn0)) {err = ENOMEM; goto error3;}
    struct dill_ipc_conn *conn1 = malloc(sizeof(struct dill_ipc_conn));
    if(dill_slow(!conn1)) {err = ENOMEM; goto error4;}
    /* Create the handles. */
    s[0] = dill_ipc_makeconn(fds[0], conn0);
    if(dill_slow(s[0] < 0)) {err = errno; goto error5;}
    conn0->mem = 0;
    s[1] = dill_ipc_makeconn(fds[1], conn1);
    if(dill_slow(s[1] < 0)) {err = errno; goto error6;}
    conn1->mem = 0;
    return 0;
error6:
    rc = dill_hclose(s[0]);
    goto error2;
error5:
    free(conn1);
error4:
    free(conn0);
error3:
    dill_fd_close(fds[0]);
error2:
    dill_fd_close(fds[1]);
error1:
    errno = err;
    return -1;
}

/******************************************************************************/
/*  Helpers                                                                   */
/******************************************************************************/

static int dill_ipc_resolve(const char *addr, struct sockaddr_un *su) {
    dill_assert(su);
    if(strlen(addr) >= sizeof(su->sun_path)) {errno = ENAMETOOLONG; return -1;}
    su->sun_family = AF_UNIX;
    strncpy(su->sun_path, addr, sizeof(su->sun_path));
    return 0;
}

static int dill_ipc_makeconn(int fd, void *mem) {
    /* Create the object. */
    struct dill_ipc_conn *self = (struct dill_ipc_conn*)mem;
    self->hvfs.query = dill_ipc_hquery;
    self->hvfs.close = dill_ipc_hclose;
    self->bvfs.bsendl = dill_ipc_bsendl;
    self->bvfs.brecvl = dill_ipc_brecvl;
    self->fd = fd;
    dill_fd_initrxbuf(&self->rxbuf);
    self->busy = 0;
    self->indone = 0;
    self->outdone = 0;
    self->inerr = 0;
    self->outerr = 0;
    self->mem = 1;
    /* Create the handle. */
    return dill_hmake(&self->hvfs);
}

