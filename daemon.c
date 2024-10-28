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
#include "discdb.h"

static char *default_configfs = "/sys/kernel/config/nvmet";
static char *default_dbfile = "nvme_discdb.sqlite";

void *inotify_loop(void *);

int stopped = 0;
int tcp_debug = 0;
int cmd_debug = 0;

int parse_opts(struct etcd_cdc_ctx *ctx, int argc, char *argv[])
{
	struct option getopt_arg[] = {
		{"configfs", required_argument, 0, 'c'},
		{"port", required_argument, 0, 'p'},
		{"tls", no_argument, 0, 't'},
		{"nqn", required_argument, 0, 'n'},
		{"verbose", no_argument, 0, 'v'},
	};
	char c;
	int getopt_ind;

	while ((c = getopt_long(argc, argv, "c:e:n:p:st:v",
				getopt_arg, &getopt_ind)) != -1) {
		switch (c) {
		case 'c':
			ctx->configfs = optarg;
			break;
		case 'n':
			strcpy(ctx->host.hostnqn, optarg);
			break;
		case 'p':
			ctx->port = atoi(optarg);
			break;
		case 't':
			ctx->tls++;
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
	int signal_fd, ret = 0;
	pthread_t inotify_thread;
	pthread_attr_t pthread_attr;

	ctx = malloc(sizeof(*ctx));
	if (!ctx) {
		fprintf(stderr, "cannot allocate context\n");
		exit(1);
	}
	ctx->configfs = default_configfs;
	ctx->ttl = 10;
	ctx->dbfile = default_dbfile;
	ctx->port = 8009;
	strcpy(ctx->host.hostnqn, NVME_DISC_SUBSYS_NAME);

	parse_opts(ctx, argc, argv);

	if (ctx->debug)
		cmd_debug = 1;
	if (ctx->debug > 1)
		tcp_debug = 1;

	if (discdb_open(ctx->dbfile)) {
		ret = 1;
		goto out_free_ctx;
	}

	if (discdb_add_host(&ctx->host) < 0) {
		fprintf(stderr, "failed to insert default host %s\n",
			ctx->host.hostnqn);
		goto out_close_db;
	}
	pthread_attr_init(&pthread_attr);
	ret = pthread_create(&inotify_thread, &pthread_attr,
			     inotify_loop, ctx);
	pthread_attr_destroy(&pthread_attr);
	if (ret) {
		inotify_thread = 0;
		fprintf(stderr, "failed to create inotify pthread: %d\n", ret);
		ret = 0;
		goto out_del_host;
	}

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGTERM);

	if (pthread_sigmask(SIG_BLOCK, &sigmask, NULL) < 0) {
		fprintf(stderr, "Couldn't block signals, error %d\n", errno);
		ret = 1;
		pthread_kill(inotify_thread, SIGTERM);
		goto out_join;
	}
	signal_fd = signalfd(-1, &sigmask, 0);
	if (signal_fd < 0) {
		fprintf(stderr, "Couldn't setup signal fd, error %d\n", errno);
		ret = 1;
		pthread_kill(inotify_thread, SIGTERM);
		goto out_join;
	}

	for (;;) {
		int rlen, ret;
		fd_set rfd;
		struct timeval tmo;

		FD_ZERO(&rfd);
		FD_SET(signal_fd, &rfd);
		tmo.tv_sec = ctx->ttl / 5;
		tmo.tv_usec = 0;
		ret = select(signal_fd + 1, &rfd, NULL, NULL, &tmo);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "select returned %d\n", errno);
			break;
		}
		if (ret == 0)
			continue;
		if (FD_ISSET(signal_fd, &rfd)) {
			struct signalfd_siginfo fdsi;

			rlen = read(signal_fd, &fdsi, sizeof(fdsi));
			if (rlen != sizeof(fdsi)) {
				fprintf(stderr, "Couldn't read siginfo\n");
				ret = 1;
				break;
			}
			if (fdsi.ssi_signo == SIGINT ||
			    fdsi.ssi_signo == SIGTERM) {
				fprintf(stderr,
					"signal %d received, terminating\n",
					fdsi.ssi_signo);
				stopped = 1;
				pthread_kill(inotify_thread, SIGTERM);
				break;
			}
		}
	}

	close(signal_fd);
out_join:
	pthread_join(inotify_thread, NULL);
out_del_host:
	discdb_del_host(&ctx->host);
out_close_db:
	discdb_close(ctx->dbfile);
out_free_ctx:
	free(ctx);
	return ret;
}
