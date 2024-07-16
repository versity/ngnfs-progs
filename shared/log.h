/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LOG_H
#define NGNFS_SHARED_LOG_H

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>

#define log(fmt, args...) \
	dprintf(STDOUT_FILENO, fmt"\n", ##args)

#define ENOF		"%s (errno %d)"
#define ENOA(eno)	strerror(eno), eno

#define IPV4F		"%u.%u.%u.%u:%u"
#define IPV4A(addr)					\
	ntohl((addr)->sin_addr.s_addr) >> 24,		\
	(ntohl((addr)->sin_addr.s_addr) >> 16) & 0xff,	\
	(ntohl((addr)->sin_addr.s_addr) >> 8) & 0xff,	\
	ntohl((addr)->sin_addr.s_addr) & 0xff,		\
	ntohs((addr)->sin_port)

#endif
