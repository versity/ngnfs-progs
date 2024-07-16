/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_OPTIONS_H
#define NGNFS_SHARED_OPTIONS_H

#include <getopt.h>

enum {
	optional_opt = 0,
	required_opt = 1,
};

struct option_more {
	struct option longopt;
	char *arg;
	char *desc;
	unsigned required:1,
		 _given:1;	/* remaining internal flags used by parsing, not specification */
};

typedef int (*opt_parse_t)(int c, char *str, void *arg);

int getopt_long_more(int argc, char *const argv[], struct option_more *moreopts, size_t nr,
		     opt_parse_t, void *arg);

#endif
