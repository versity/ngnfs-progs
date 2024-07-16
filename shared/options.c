/* SPDX-License-Identifier: GPL-2.0 */

#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <ctype.h>
#include <assert.h>

#include "shared/log.h"
#include "shared/options.h"

#define for_each_more(more, i, moreopts, nr) \
	for (i = 0, more = moreopts; i < nr; i++, more++)

static int prepare_optstr(char **optstr, struct option_more *moreopts, size_t nr)
{
	struct option_more *more;
	char *str;
	int ret;
	int o;
	int i;

	str = malloc(4 + (nr * 3) + 1);
	if (str == NULL) {
		ret = -errno;
		log("error allocating option string");
		goto out;
	}

	o = 0;
	str[o++] = '+';
	str[o++] = ':';
	str[o++] = 'h';
	str[o++] = '?';

	for_each_more(more, i, moreopts, nr) {
		str[o++] = more->longopt.val;
		if (more->longopt.has_arg == required_argument) {
			str[o++] = ':';
		} else if (more->longopt.has_arg == optional_argument) {
			str[o++] = ':';
			str[o++] = ':';
		}
	}
	str[o++] = '\0';

	*optstr = str;
	ret = 0;
out:
	return ret;
}

static int prepare_longopts(struct option **longopts, struct option_more *moreopts, size_t nr)
{
	struct option_more *more;
	struct option *opt;
	int ret;
	int i;

	opt = calloc(nr + 1, sizeof(opt[0]));
	if (!longopts) {
		ret = -ENOMEM;
		goto out;
	}

	for_each_more(more, i, moreopts, nr)
		opt[i] = more->longopt;
	opt[nr] = (struct option) { .name = NULL };

	*longopts = opt;
	ret = 0;
out:
	return ret;
}

static void show_help(struct option_more *moreopts, size_t nr)
{
	struct option_more *more;
	int i;

	for_each_more(more, i, moreopts, nr) {
		log("    (-%c | --%s) %s",
			more->longopt.val, more->longopt.name, more->arg ? more->arg : "");
		log("        %s", more->desc);
	}
}

static struct option_more *get_more(struct option_more *moreopts, size_t nr, int c)
{
	struct option_more *more;
	int i;

	for_each_more(more, i, moreopts, nr) {
		if (more->longopt.val == c)
			return &moreopts[i];
	}

	return NULL;
}

/*
 * A convenience wrapper around getopt_long() which constructs the
 * optstring from a richer option struct array, handles logging and
 * help, and can check additional constraints.  The caller's func is
 * called with each parsed option.  Effectively this implements the
 * boilerplate you'd see in a bunch of getopt callers with the option
 * switch implemented in the caller's func.
 */
int getopt_long_more(int argc, char *const argv[], struct option_more *moreopts, size_t nr,
		     opt_parse_t func, void *arg)
{
	struct option *longopts = NULL;
	struct option_more *more;
	char *optstr = NULL;
	int ret;
	int i;
	int c;

	ret = prepare_optstr(&optstr, moreopts, nr) ?:
	      prepare_longopts(&longopts, moreopts, nr);
	if (ret < 0)
		goto out;

	opterr = 0;
	ret = 0;

	while ((c = getopt_long(argc, argv, optstr, longopts, NULL)) != -1) {

		more = get_more(moreopts, nr, c == ':' ? optopt : c);
		ret = -EINVAL;

		if (c == 'h' || (c == '?' && optopt == 0)) {
			;

		} else if (c == '?') {
			log("unrecognized option -%c (0x%x)",
			    isprint(optopt) ? optopt : '?', optopt);

		} else if (c == ':') {
			log("option --%s (-%c) missing required argument",
			    more->longopt.name, more->longopt.val);

		} else {
			ret = func(c, optarg, arg);
			if (ret < 0)
				log("error parsing --%s (-%c) option: "ENOF,
				    more->longopt.name, c, ENOA(-ret));
			else
				more->_given = 1;
		}

		if (ret < 0)
			goto help;
	}

	for_each_more(more, i, moreopts, nr) {
		if (more->required && !more->_given) {
			log("missing required --%s (-%c) option",
			    more->longopt.name, more->longopt.val);
			ret = -EINVAL;
			goto help;
		}
	}

	ret = 0;
help:
	if (ret < 0)
		show_help(moreopts, nr);

out:
	free(optstr);
	free(longopts);
	return ret;
}
