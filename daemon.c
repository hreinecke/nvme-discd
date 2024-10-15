/*
 * nvmet_etcd.c
 * decentralized NVMe discovery controller
 *
 * Copyright (c) 2021 Hannes Reinecke <hare@suse.de>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software Foundation,
 *  Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <getopt.h>
#include <sys/types.h>
#include <fcntl.h>

#include <sys/inotify.h>

#include "common.h"

static char *default_configfs = "/sys/kernel/config/nvmet";

#define INOTIFY_BUFFER_SIZE 8192

int inotify_fd;
static int signal_fd;

void inotify_loop(struct etcd_cdc_ctx *, int, int);

int parse_opts(struct etcd_cdc_ctx *ctx, int argc, char *argv[])
{
	struct option getopt_arg[] = {
		{"configfs", required_argument, 0, 'c'},
		{"port", required_argument, 0, 'p'},
		{"host", required_argument, 0, 'h'},
		{"ssl", no_argument, 0, 's'},
		{"verbose", no_argument, 0, 'v'},
	};
	char c;
	int getopt_ind;

	while ((c = getopt_long(argc, argv, "c:e:p:h:st:v",
				getopt_arg, &getopt_ind)) != -1) {
		switch (c) {
		case 'c':
			ctx->configfs = optarg;
			break;
		case 'e':
			ctx->prefix = optarg;
			break;
		case 'h':
			ctx->host = optarg;
			break;
		case 'p':
			ctx->port = atoi(optarg);
			break;
		case 's':
			ctx->proto = "https";
			break;
		case 'v':
			ctx->debug++;
			break;
		}
	}
	return 0;
}

int main (int argc, char *argv[])
{
	struct etcd_cdc_ctx *ctx;
	sigset_t sigmask;

	ctx = malloc(sizeof(*ctx));
	if (!ctx) {
		fprintf(stderr, "cannot allocate context\n");
		exit(1);
	}
	ctx->configfs = default_configfs;
	ctx->ttl = 10;

	parse_opts(ctx, argc, argv);

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGTERM);

	if (sigprocmask(SIG_BLOCK, &sigmask, NULL) < 0) {
		fprintf(stderr, "Couldn't block signals, error %d\n", errno);
		free(ctx);
		exit(1);
	}
	signal_fd = signalfd(-1, &sigmask, 0);
	if (signal_fd < 0) {
		fprintf(stderr, "Couldn't setup signal fd, error %d\n", errno);
		free(ctx);
		exit(1);
	}
	inotify_fd = inotify_init();
	if (inotify_fd < 0) {
		fprintf(stderr, "Could not setup inotify, error %d\n", errno);
		free(ctx);
		exit(1);
	}

	ctx->genctr = 1;

	inotify_loop(ctx, inotify_fd, signal_fd);

	close(inotify_fd);
	close(signal_fd);
	free(ctx);
	return 0;
}
