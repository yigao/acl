#include "stdafx.h"
#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <dlfcn.h>
#include <sys/epoll.h>
#include "event.h"
#include "event_epoll.h"

typedef int (*epoll_create_fn)(int);
typedef int (*epoll_wait_fn)(int, struct epoll_event *,int, int);
typedef int (*epoll_ctl_fn)(int, int, int, struct epoll_event *);

static epoll_create_fn __sys_epoll_create = NULL;
static epoll_wait_fn   __sys_epoll_wait   = NULL;
static epoll_ctl_fn    __sys_epoll_ctl    = NULL;

void hook_epoll(void)
{
	static acl_pthread_mutex_t __lock = PTHREAD_MUTEX_INITIALIZER;
	static int __called = 0;

	(void) acl_pthread_mutex_lock(&__lock);

	if (__called) {
		(void) acl_pthread_mutex_unlock(&__lock);
		return;
	}

	__called++;

	__sys_epoll_create = (epoll_create_fn) dlsym(RTLD_NEXT, "epoll_create");
	acl_assert(__sys_epoll_create);

	__sys_epoll_wait   = (epoll_wait_fn) dlsym(RTLD_NEXT, "epoll_wait");
	acl_assert(__sys_epoll_wait);

	__sys_epoll_ctl    = (epoll_ctl_fn) dlsym(RTLD_NEXT, "epoll_ctl");
	acl_assert(__sys_epoll_ctl);

	(void) acl_pthread_mutex_unlock(&__lock);
}

/****************************************************************************/

typedef struct EVENT_EPOLL {
	EVENT event;
	int   epfd;
	struct epoll_event *events;
	int   size;
} EVENT_EPOLL;

static void epoll_event_free(EVENT *ev)
{
	EVENT_EPOLL *ep = (EVENT_EPOLL *) ev;

	close(ep->epfd);
	acl_myfree(ep->events);
	acl_myfree(ep);
}

static void epoll_enable_read(EVENT *ev, FILE_EVENT *fe, event_proc *proc)
{
	assert((fe->oper & EVENT_ADD_READ) == 0);
	if (fe->oper & EVENT_DEL_READ) {
		fe->oper &= ~EVENT_DEL_READ;
	} else if (fe->oper == 0) {
		acl_ring_prepend(&ev->events, &fe->me);
	}

	if (!(fe->mask & EVENT_READ)) {
		fe->oper |= EVENT_ADD_READ;
	}
	fe->r_proc = proc;
}

static void epoll_enable_write(EVENT *ev, FILE_EVENT *fe, event_proc *proc)
{
	if (fe->oper & EVENT_DEL_WRITE) {
		fe->oper &= ~EVENT_DEL_WRITE;
	} else if (fe->oper == 0) {
		acl_ring_prepend(&ev->events, &fe->me);
	}

	if (!(fe->mask & EVENT_WRITE))
		fe->oper |= EVENT_ADD_WRITE;
	fe->w_proc = proc;
}

static void epoll_disable_read(EVENT *ev, FILE_EVENT *fe)
{
	if (fe->oper & EVENT_ADD_READ) {
		fe->oper &=~EVENT_ADD_READ;
	} else if (fe->oper == 0) {
		acl_ring_prepend(&ev->events, &fe->me);
	}

	if (fe->mask & EVENT_READ) {
		fe->oper |= EVENT_DEL_READ;
	}
	fe->r_proc  = NULL;
}

static void epoll_disable_write(EVENT *ev, FILE_EVENT *fe)
{
	if (fe->oper & EVENT_ADD_WRITE) {
		fe->oper &= ~EVENT_ADD_WRITE;
	} else if (fe->oper == 0) {
		acl_ring_prepend(&ev->events, &fe->me);
	}

	if (fe->mask & EVENT_WRITE)
		fe->oper |= EVENT_DEL_WRITE;
	fe->w_proc = NULL;
}

static int epoll_event_add_read(EVENT_EPOLL *ep, FILE_EVENT *fe)
{
	struct epoll_event ee;
	int op;

	if ((fe->mask & EVENT_READ))
		return 0;

	ee.events   = 0;
	ee.data.u32 = 0;
	ee.data.u64 = 0;
	ee.data.ptr = fe;

	ee.events |= EPOLLIN;
	if (fe->mask & EVENT_WRITE) {
		ee.events |= EPOLLOUT;
		op = EPOLL_CTL_MOD;
	} else {
		op = EPOLL_CTL_ADD;
	}

	if (__sys_epoll_ctl(ep->epfd, op, fe->fd, &ee) == -1) {
		acl_msg_error("%s(%d): epoll_ctl error %s, epfd=%d, fd=%d",
			__FUNCTION__, __LINE__, acl_last_serror(),
			ep->epfd, fe->fd);
		return -1;
	}

	fe->mask |= EVENT_READ;
	return 0;
}

static int epoll_event_add_write(EVENT_EPOLL *ep, FILE_EVENT *fe)
{
	struct epoll_event ee;
	int op;

	ee.events   = 0;
	ee.data.u32 = 0;
	ee.data.u64 = 0;
	ee.data.ptr = fe;

	ee.events |= EPOLLOUT;
	if (fe->mask & EVENT_READ) {
		ee.events |= EPOLLIN;
		op = EPOLL_CTL_MOD;
	} else {
		op = EPOLL_CTL_ADD;
	}

	if (__sys_epoll_ctl(ep->epfd, op, fe->fd, &ee) == -1) {
		acl_msg_error("%s(%d): epoll_ctl error %s, epfd=%d, fd=%d",
			__FUNCTION__, __LINE__, acl_last_serror(),
			ep->epfd, fe->fd);
		return -1;
	}

	fe->mask |= EVENT_WRITE;
	return 0;
}

static int epoll_event_del_read(EVENT_EPOLL *ep, FILE_EVENT *fe)
{
	struct epoll_event ee;
	int op;

	ee.events   = 0;
	ee.data.u64 = 0;
	ee.data.fd  = 0;
	ee.data.ptr = fe;

	if (fe->mask & EVENT_WRITE) {
		ee.events = EPOLLOUT;
		op = EPOLL_CTL_MOD;
	} else {
		op = EPOLL_CTL_DEL;
	}

	if (__sys_epoll_ctl(ep->epfd, op, fe->fd, &ee) < 0) {
		if (errno == EEXIST)
			return 0;

		acl_msg_error("%s(%d), epoll_ctl error: %s, epfd=%d, fd=%d",
			__FUNCTION__, __LINE__, acl_last_serror(),
			ep->epfd, fe->fd);
		return -1;
	}

	fe->mask &= ~EVENT_READ;
	return 0;
}

static int epoll_event_del_write(EVENT_EPOLL *ep, FILE_EVENT *fe)
{
	struct epoll_event ee;
	int op;

	ee.events   = 0;
	ee.data.u64 = 0;
	ee.data.fd  = 0;
	ee.data.ptr = fe;

	if (fe->mask & EVENT_READ) {
		ee.events = EPOLLIN;
		op = EPOLL_CTL_MOD;
	} else {
		op = EPOLL_CTL_DEL;
	}

	if (__sys_epoll_ctl(ep->epfd, op, fe->fd, &ee) < 0) {
		if (errno == EEXIST)
			return 0;

		acl_msg_error("%s(%d), epoll_ctl error: %s, efd=%d, fd=%d",
			__FUNCTION__, __LINE__, acl_last_serror(),
			ep->epfd, fe->fd);
		return -1;
	}

	fe->mask &= ~EVENT_WRITE;
	return 0;
}

static void epoll_event_prepare(EVENT *ev)
{
	ACL_RING   *r;
	FILE_EVENT *fe;

	while ((r = acl_ring_pop_head(&ev->events)) != NULL) {
		int n = 0;
		fe = acl_ring_to_appl(r, FILE_EVENT, me);

		if (fe->oper & EVENT_ADD_READ) {
			epoll_event_add_read((EVENT_EPOLL *) ev, fe);
			n++;
		}
		if (fe->oper & EVENT_ADD_WRITE) {
			epoll_event_add_write((EVENT_EPOLL *) ev, fe);
			n++;
		}
		if (fe->oper & EVENT_DEL_READ) {
			epoll_event_del_read((EVENT_EPOLL *) ev, fe);
			n++;
		}
		if (fe->oper & EVENT_DEL_WRITE) {
			epoll_event_del_write((EVENT_EPOLL *) ev, fe);
			n++;
		}

		fe->oper = 0;
	}
}

static int epoll_event_loop(EVENT *ev, int timeout)
{
	EVENT_EPOLL *ep = (EVENT_EPOLL *) ev;
	struct epoll_event *ee;
	FILE_EVENT *fe;
	int n, i;

	if (__sys_epoll_wait == NULL) {
		hook_epoll();
	}

	epoll_event_prepare(ev);

	n = __sys_epoll_wait(ep->epfd, ep->events, ep->size, timeout);

	if (n == 0) {
		return n;
	} else if (n < 0) {
		acl_msg_error("%s(%d): epoll_wait error %s",
			__FUNCTION__, __LINE__, acl_last_serror());
		return n;
	}

	for (i = 0; i < n; i++) {
		ee = &ep->events[i];
		fe = (FILE_EVENT *) ee->data.ptr;

		if (ee->events & EPOLLIN && fe && fe->r_proc) {
			fe->r_proc(ev, fe);
		}

		if (ee->events & EPOLLOUT && fe && fe->w_proc) {
			fe->w_proc(ev, fe);
		}
	}

	return n;
}

static int epoll_event_handle(EVENT *ev)
{
	EVENT_EPOLL *ep = (EVENT_EPOLL *) ev;

	return ep->epfd;
}

static const char *epoll_event_name(void)
{
	return "epoll";
}

EVENT *event_epoll_create(int size)
{
	EVENT_EPOLL *ep = (EVENT_EPOLL *) acl_mymalloc(sizeof(EVENT_EPOLL));

	ep->events = (struct epoll_event *)
		acl_mymalloc(sizeof(struct epoll_event) * size);
	ep->size   = size;

	if (__sys_epoll_create == NULL) {
		hook_epoll();
	}

	ep->epfd = __sys_epoll_create(1024);
	acl_assert(ep->epfd >= 0);

	ep->event.name   = epoll_event_name;
	ep->event.handle = epoll_event_handle;
	ep->event.free   = epoll_event_free;

	ep->event.event_loop    = epoll_event_loop;
	ep->event.enable_read   = epoll_enable_read;
	ep->event.enable_write  = epoll_enable_write;
	ep->event.disable_read  = epoll_disable_read;
	ep->event.disable_write = epoll_disable_write;

	return (EVENT*) ep;
}
