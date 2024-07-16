/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_PARSE_H
#define NGNFS_SHARED_PARSE_H

int parse_ull(unsigned long long *ull, char *str, unsigned long long least,
	      unsigned long long most);
int parse_ll(long long *ll, char *str, long long least, long long most);
int parse_ipv4_addr_port(struct sockaddr_in *sin, char *str);

#endif
