/*
 * ============================================================================
 *
 *       Filename:  link.c
 *
 *    Description:  
 *
 *        Version:  1.0
 *        Created:  2014年09月02日 14时27分53秒
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  jianxi sun (jianxi), ycsunjane@gmail.com
 *   Organization:  
 *
 * ============================================================================
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/epoll.h>
#include <errno.h>
#include <fcntl.h>

#include "log.h"
#include "link.h"

static pthread_mutex_t sockarr_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sockarr_t *__head = NULL;
static struct sockarr_t **__tail = &__head;
#define SOCKARR_LOCK() 	 			\
do { 						\
	sys_lock("sockarr lock\n"); 		\
	pthread_mutex_lock(&sockarr_lock); 	\
}while(0)

#define SOCKARR_UNLOCK() 			\
do { 						\
	sys_lock("sockarr unlock\n"); 		\
	pthread_mutex_unlock(&sockarr_lock); 	\
}while(0)

int epl;

static int net_nonblock(int socket)
{
	int flags;

	flags = fcntl(socket, F_GETFL, 0);
	if(flags < 0) {
		sys_err("Get socket flags failed: %s(%d)\n", 
			strerror(errno), errno);
		return -1;
	}

	if(fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0) {
		sys_err("Set socket flags failed: %s(%d)\n", 
			strerror(errno), errno);
		return -1;
	}

	return 0;
}
void net_epoll_init()
{
	epl = epoll_create1(EPOLL_CLOEXEC);
	if(epl < 0) {
		sys_err("Create epoll failed: %s(%d)\n", 
			strerror(epl), epl);
		exit(-1);
	}

	return;
}

#define EPOLLNUM 	(10)
static struct epoll_event epollarr[EPOLLNUM];

void * net_recv(void *arg)
{
	int ret, i;
	struct sockarr_t *sockarr;

	while(1) {
		ret = epoll_wait(epl, epollarr, EPOLLNUM, -1);
		if(ret < 0) {
			if(errno == EINTR)
				continue;
			sys_err("Epoll wait failed: %s(%d)\n", 
				strerror(errno), errno);
			exit(-1);
		}

		for(i = 0; i < ret; i++) {
			sockarr = epollarr[i].data.ptr;
			sockarr->retevents = epollarr[i].events;
			if(epollarr[i].events & EPOLLRDHUP) {
				sys_debug("Epool get err: %s %s(%d)\n",
					"EPOLLRDHUP", strerror(errno), errno);
			} else if(epollarr[i].events & EPOLLERR) {
				sys_debug("Epool get err: %s %s(%d)\n",
					"EPOLLERR", strerror(errno), errno);
			} else if(epollarr[i].events & EPOLLHUP) {
				sys_debug("Epool get err: %s %s(%d)\n",
					"EPOLLHUP", strerror(errno), errno);
			}

			sockarr->func(sockarr);
		}
	}
	return NULL;	
}

int net_send(int proto, int sock, char *dmac, char *msg, int size)
{
	switch(proto) {
	case MSG_PROTO_ETH:
		sys_debug("Send packet through datalink layer\n");
		return dll_sendpkt(dmac, msg, size);
	case MSG_PROTO_TCP:
		if(sock < 0) {
			sys_err("Invalid socket: %d\n", sock);
			return -1;
		}

		sys_debug("Send packet through net layer, sock: %d, msg: %p, size: %d\n", 
			sock, msg, size);
		struct nettcp_t net;
		net.sock = sock;
		return tcp_sendpkt(&net, msg, size);
	default:
		sys_err("Invalid protocol\n");
		return -1;
	}
	return -1;
}

struct sockarr_t *
insert_sockarr(int sock, void *(*func) (void *), void *arg)
{

	int ret;
	ret = net_nonblock(sock);
	if(ret < 0)
		return NULL;

	struct sockarr_t *__sock = malloc(sizeof(struct sockarr_t));
	if(__sock == NULL) {
		sys_err("Malloc sockarr failed: %s\n", strerror(errno));
		exit(-1);
	}
	__sock->ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
	__sock->ev.data.ptr = __sock;
	__sock->sock = sock;
	__sock->func = func;
	__sock->arg = arg;
	__sock->next = NULL;

	/* add in epoll */
	ret = epoll_ctl(epl, EPOLL_CTL_ADD, 
		__sock->sock, &__sock->ev);
	if(ret < 0) {
		sys_err("Add epoll fd failed: %s(%d)\n", 
			strerror(errno), errno);
		exit(-1);
	}

	SOCKARR_LOCK();
	*__tail = __sock;
	__tail = &__sock->next;
	SOCKARR_UNLOCK();

	return __sock;
}

int delete_sockarr(int sock)
{
	struct sockarr_t *cur;
	struct sockarr_t **ppre;

	SOCKARR_LOCK();
	cur = __head;
	ppre = &__head;

	int ret;
	while(cur) {
		if(cur->sock == sock) {
			*ppre = cur->next;
			if(&cur->next == __tail)
				__tail = ppre;
			SOCKARR_UNLOCK();

			ret = epoll_ctl(epl, EPOLL_CTL_DEL, sock, NULL);
			if(ret < 0)
				sys_err("Delete epoll sock: %d failed: %s(%d)\n", 
					sock, strerror(errno), errno);
			close(sock);
			free(cur);
			return 0;
		}
		ppre = &cur->next;
		cur = cur->next;
	}
	SOCKARR_UNLOCK();
	return 0;
}

