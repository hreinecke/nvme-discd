/*
 * nvmet_inotify.c
 * inotify watcher for nvmet configfs
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
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/signalfd.h>
#include <fcntl.h>

#include <sys/inotify.h>

#include "list.h"
#include "common.h"

#define INOTIFY_BUFFER_SIZE 8192

LIST_HEAD(dir_watcher_list);

int debug_inotify = 1;

enum watcher_type {
	TYPE_HOST_DIR,		/* hosts */
	TYPE_HOST,		/* hosts/<host> */
	TYPE_PORT_DIR,		/* ports */
	TYPE_PORT,		/* ports/<port> */
	TYPE_PORT_ATTR,		/* ports/<port>/addr_<attr> */
	TYPE_PORT_SUBSYS_DIR,	/* ports/<port>/subsystems */
	TYPE_PORT_SUBSYS,	/* ports/<port>/subsystems/<subsys> */
	TYPE_SUBSYS_DIR,	/* subsystems */
	TYPE_SUBSYS,		/* subsystems/<subsys> */
	TYPE_SUBSYS_HOSTS_DIR,	/* subsystems/<subsys>/allowed_hosts */
	TYPE_SUBSYS_HOST,	/* subsystems/<subsys>/allowed_hosts/<host> */
};

struct dir_watcher {
	struct list_head entry;
	enum watcher_type type;
	int wd;
	char dirname[FILENAME_MAX];
};

/* TYPE_HOST */
struct nvmet_host {
	struct dir_watcher watcher;
	char hostnqn[256];
};

/* TYPE_PORT */
struct nvmet_port {
	struct dir_watcher watcher;
	char port_id[256];
	char trtype[256];
	char traddr[256];
	char trsvcid[256];
	char adrfam[256];
	char treq[256];
	char tsas[256];
};

/* TYPE_PORT_ATTR */
struct nvmet_port_attr {
	struct dir_watcher watcher;
	struct nvmet_port *port;
	char attr[256];
};

/* TYPE_SUBSYS */
struct nvmet_subsys {
	struct dir_watcher watcher;
	char subsysnqn[256];
};

/* TYPE_PORT_SUBSYS */
struct nvmet_port_subsys {
	struct dir_watcher watcher;
	struct nvmet_port *port;
	char subsysnqn[256];
};

/* TYPE_SUBSYS_HOST */
struct nvmet_subsys_host {
	struct dir_watcher watcher;
	struct nvmet_subsys *subsys;
	char hostnqn[256];
};

static struct nvmet_port *find_port_from_subsys(char *port_subsys_dir)
{
	struct dir_watcher *watcher;

	list_for_each_entry(watcher, &dir_watcher_list, entry) {
		if (watcher->type != TYPE_PORT)
			continue;
		if (strncmp(watcher->dirname, port_subsys_dir,
			    strlen(watcher->dirname)))
			continue;
		return container_of(watcher, struct nvmet_port, watcher);
	}
	fprintf(stderr, "No port found for subsys %s\n", port_subsys_dir);
	return NULL;
}

static struct nvmet_subsys *find_subsys(char *subnqn)
{
	struct dir_watcher *watcher;
	struct nvmet_subsys *subsys;

	list_for_each_entry(watcher, &dir_watcher_list, entry) {
		if (watcher->type != TYPE_SUBSYS)
			continue;
		subsys = container_of(watcher, struct nvmet_subsys,
				      watcher);
		if (strcmp(subsys->subsysnqn, subnqn))
			continue;
		return subsys;
	}
	if (debug_inotify)
		printf("Subsys %s not found\n", subnqn);
	return NULL;
}

static struct nvmet_subsys *find_subsys_from_host(char *subsys_host_dir)
{
	char subnqn[256], *p;

	p = strchr(subsys_host_dir, '/');
	do {
		if (!p)
			break;
		p++;
		if (!strncmp(p, "subsystems", 10)) {
			strncpy(subnqn, p + 11, 256);
			break;
		}
	} while ((p = strchr(p, '/')));
	if (!strlen(subnqn)) {
		fprintf(stderr, "Invalid subsys path %s\n", subsys_host_dir);
		return NULL;
	}
	p = strchr(subnqn, '/');
	if (p)
		*p = '\0';

	return find_subsys(subnqn);
}

static struct dir_watcher *add_watch(int fd, struct dir_watcher *watcher,
				     int flags)
{
	struct dir_watcher *tmp;

	INIT_LIST_HEAD(&watcher->entry);
	list_for_each_entry(tmp, &dir_watcher_list, entry) {
		if (tmp->type != watcher->type)
			continue;
		if (strcmp(tmp->dirname, watcher->dirname))
			continue;
		if (debug_inotify)
			printf("re-use inotify watch %d type %d for %s\n",
			       watcher->wd, watcher->type, watcher->dirname);
		return tmp;
	}
	watcher->wd = inotify_add_watch(fd, watcher->dirname, flags);
	if (watcher->wd < 0) {
		fprintf(stderr,
			"failed to add inotify watch to '%s', error %d\n",
			watcher->dirname, errno);
		return watcher;
	}
	if (debug_inotify)
		printf("add inotify watch %d type %d to %s\n",
		       watcher->wd, watcher->type, watcher->dirname);
	list_add(&watcher->entry, &dir_watcher_list);
	return 0;
}

static int remove_watch(int fd, struct dir_watcher *watcher)
{
	int ret;

	ret = inotify_rm_watch(fd, watcher->wd);
	if (ret < 0)
		fprintf(stderr, "Failed to remove inotify watch on '%s'\n",
			watcher->dirname);
	if (debug_inotify)
		printf("remove inotify watch %d type %d from '%s'\n",
		       watcher->wd, watcher->type, watcher->dirname);
	list_del_init(&watcher->entry);
	return ret;
}

static int watch_directory(int fd, char *dirname,
			   enum watcher_type type, int flags)
{
	struct dir_watcher *watcher, *tmp;

	watcher = malloc(sizeof(struct dir_watcher));
	if (!watcher) {
		fprintf(stderr, "Failed to allocate dirwatch\n");
		return -1;
	}
	strcpy(watcher->dirname, dirname);
	watcher->type = type;
	tmp = add_watch(fd, watcher, flags);
	if (tmp) {
		if (tmp == watcher)
			free(watcher);
		return -1;
	}
 	return 0;
}

static void watch_host(int fd, struct etcd_cdc_ctx *ctx,
		       char *hosts_dir, char *hostnqn)
{
	struct nvmet_host *host;
	struct dir_watcher *watcher;

	host = malloc(sizeof(struct nvmet_host));
	if (!host)
		return;

	strcpy(host->hostnqn, hostnqn);
	sprintf(host->watcher.dirname, "%s/%s",
		hosts_dir, hostnqn);
	host->watcher.type = TYPE_HOST;
	watcher = add_watch(fd, &host->watcher, IN_DELETE_SELF);
	if (watcher) {
		if (watcher == &host->watcher)
			free(host);
		return;
	}
}

static int port_read_attr(struct nvmet_port *port, char *ports_dir, char *attr)
{
	char attr_path[PATH_MAX + 1];
	char *attr_buf, *p;
	int fd, len;

	if (!strcmp(attr, "trtype"))
		attr_buf = port->trtype;
	else if (!strcmp(attr, "traddr"))
		attr_buf = port->traddr;
	else if (!strcmp(attr, "trsvcid"))
		attr_buf = port->trsvcid;
	else if (!strcmp(attr, "adrfam"))
		attr_buf = port->adrfam;
	else if (!strcmp(attr, "tsas"))
		attr_buf = port->tsas;
	else if (!strcmp(attr, "treq"))
		attr_buf = port->treq;
	else {
		fprintf(stderr, "Port %s: Invalid attribute '%s'\n",
			port->port_id, attr);
		return -1;
	}

	sprintf(attr_path, "%s/%s/addr_%s", ports_dir, port->port_id, attr);
	fd = open(attr_path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Port %s: Failed to open '%s', error %d\n",
			port->port_id, attr_path, errno);
		return -1;
	}
	len = read(fd, attr_buf, 256);
	if (len < 0)
		memset(attr_buf, 0, 256);
	else {
		p = &attr_buf[len - 1];
		if (*p == '\n')
			*p = '\0';
	}
	close(fd);

	return len;
}

static struct nvmet_port *update_port(char *ports_dir, char *port_id)
{
	struct nvmet_port *port;

	port = malloc(sizeof(struct nvmet_port));
	if (!port) {
		fprintf(stderr, "Port %s: Failed to allocate port\n",
			port_id);
		return NULL;
	}
	strcpy(port->port_id, port_id);
	port_read_attr(port, ports_dir, "trtype");
	port_read_attr(port, ports_dir, "traddr");
	port_read_attr(port, ports_dir, "trsvcid");
	port_read_attr(port, ports_dir, "adrfam");
	port_read_attr(port, ports_dir, "treq");
	port_read_attr(port, ports_dir, "tsas");
	return port;
}

static void watch_port_attr(int fd, struct etcd_cdc_ctx *ctx,
			    char *attr_path, char *attr)
{
	struct nvmet_port_attr *port_attr;
	struct dir_watcher *watcher;

	port_attr = malloc(sizeof(struct nvmet_port_attr));
	if (!port_attr) {
		fprintf(stderr, "Failed to allocate port attr watch\n");
		return;
	}
	if (strncmp(attr, "addr_", 5)) {
		fprintf(stderr, "Invalid attribute %s\n", attr);
		free(port_attr);
		return;
	}
	strcpy(port_attr->attr, attr + 5);
	strcpy(port_attr->watcher.dirname, attr_path);
	strcat(port_attr->watcher.dirname, "/");
	strcat(port_attr->watcher.dirname, attr);
	port_attr->watcher.type = TYPE_PORT_ATTR;
	watcher = add_watch(fd, &port_attr->watcher, IN_MODIFY);
	if (watcher) {
		if (watcher == &port_attr->watcher)
			free(port_attr);
	}
	gen_disc_aen(ctx);
}

static void watch_port_subsys(int fd, struct etcd_cdc_ctx *ctx,
			      char *port_subsys_dir, char *subsysnqn)
{
	struct nvmet_port_subsys *subsys;
	struct dir_watcher *watcher;

	subsys = malloc(sizeof(struct nvmet_port_subsys));
	if (!subsys) {
		fprintf(stderr, "Failed to allocate subsys %s\n",
			subsysnqn);
		return;
	}
	strcpy(subsys->subsysnqn, subsysnqn);
	sprintf(subsys->watcher.dirname, "%s/%s",
		port_subsys_dir, subsysnqn);
	subsys->watcher.type = TYPE_PORT_SUBSYS;
	watcher = add_watch(fd, &subsys->watcher, IN_DELETE_SELF);
	if (watcher) {
		if (watcher == &subsys->watcher)
			free(subsys);
		return;
	}
	subsys->port = find_port_from_subsys(port_subsys_dir);
	gen_disc_aen(ctx);
}
	
static void watch_port(int fd, struct etcd_cdc_ctx *ctx,
		       char *ports_dir, char *port_id)
{
	struct nvmet_port *port;
	struct dir_watcher *watcher;
	char subsys_dir[PATH_MAX + 1];
	DIR *sd;
	struct dirent *se;

	port = update_port(ports_dir, port_id);
	if (!port)
		return;

	strcpy(subsys_dir, ports_dir);
	strcat(subsys_dir, "/");
	strcat(subsys_dir, port_id);
	strcpy(port->watcher.dirname, subsys_dir);
	port->watcher.type = TYPE_PORT;
	watcher = add_watch(fd, &port->watcher, IN_DELETE_SELF);
	if (watcher) {
		if (watcher == &port->watcher)
			free(port);
		return;
	}

	sd = opendir(subsys_dir);
	if (!sd) {
		fprintf(stderr, "Cannot open %s\n", subsys_dir);
		return;
	}
	while((se = readdir(sd))) {
		if (strncmp(se->d_name, "addr_", 5))
			continue;
		watch_port_attr(fd, ctx, subsys_dir, se->d_name);
	}
	closedir(sd);

	strcat(subsys_dir, "/subsystems");
	watch_directory(fd, subsys_dir, TYPE_PORT_SUBSYS_DIR,
			IN_CREATE | IN_DELETE | IN_DELETE_SELF);

	sd = opendir(subsys_dir);
	if (!sd) {
		fprintf(stderr, "Cannot open %s\n", subsys_dir);
		return;
	}
	while ((se = readdir(sd))) {
		if (!strcmp(se->d_name, ".") ||
		    !strcmp(se->d_name, ".."))
			continue;
		watch_port_subsys(fd, ctx, subsys_dir, se->d_name);
	}
	closedir(sd);
}

static void watch_subsys_host(int fd, struct etcd_cdc_ctx *ctx,
			      char *hosts_dir, char *hostnqn)
{
	struct nvmet_subsys_host *host;
	struct dir_watcher *watcher;

	host = malloc(sizeof(struct nvmet_subsys_host));
	if (!host) {
		fprintf(stderr, "Cannot allocate %s\n", hostnqn);
		return;
	}
	strcpy(host->hostnqn, hostnqn);
	sprintf(host->watcher.dirname, "%s/%s",
		hosts_dir, hostnqn);
	host->watcher.type = TYPE_SUBSYS_HOST;

	watcher = add_watch(fd, &host->watcher, IN_DELETE_SELF);
	if (watcher) {
		if (watcher == &host->watcher)
			free(host);
		return;
	}
	host->subsys = find_subsys_from_host(hosts_dir);
	gen_disc_aen(ctx);
}

static void watch_subsys(int fd, struct etcd_cdc_ctx *ctx,
			 char *subsys_dir, char *subnqn)
{
	struct nvmet_subsys *subsys;
	struct dir_watcher *watcher;
	char ah_dir[PATH_MAX + 1];
	DIR *ad;
	struct dirent *ae;

	subsys = malloc(sizeof(struct nvmet_subsys));
	if (!subsys)
		return;
	strcpy(subsys->subsysnqn, subnqn);

	sprintf(subsys->watcher.dirname, "%s/%s",
		subsys_dir, subnqn);
	subsys->watcher.type = TYPE_SUBSYS;
	watcher = add_watch(fd, &subsys->watcher, IN_DELETE_SELF);
	if (watcher) {
		if (watcher == &subsys->watcher) {
			free(subsys);
			return;
		}
	}

	sprintf(ah_dir, "%s/%s/allowed_hosts",
		subsys_dir, subnqn);
	watch_directory(fd, ah_dir, TYPE_SUBSYS_HOSTS_DIR,
			IN_CREATE | IN_DELETE | IN_DELETE_SELF);
	ad = opendir(ah_dir);
	if (!ad) {
		fprintf(stderr, "Cannot open %s\n", ah_dir);
		return;
	}
	while ((ae = readdir(ad))) {
		if (!strcmp(ae->d_name, ".") ||
		    !strcmp(ae->d_name, ".."))
			continue;
		watch_subsys_host(fd, ctx, ah_dir, ae->d_name);
	}
	closedir(ad);
}

static void
display_inotify_event(struct inotify_event *ev)
{
	if (!debug_inotify)
		return;
	printf("inotify wd = %d; ", ev->wd);
	if (ev->cookie > 0)
		printf("cookie = %4d; ", ev->cookie);

	printf("mask = ");

	if (ev->mask & IN_ISDIR)
		printf("IN_ISDIR ");

	if (ev->mask & IN_CREATE)
		printf("IN_CREATE ");

	if (ev->mask & IN_DELETE)
		printf("IN_DELETE ");

	if (ev->mask & IN_DELETE_SELF)
		printf("IN_DELETE_SELF ");

	if (ev->mask & IN_MODIFY)
		printf("IN_MODIFY ");

	if (ev->mask & IN_MOVE_SELF)
		printf("IN_MOVE_SELF ");
	if (ev->mask & IN_MOVED_FROM)
		printf("IN_MOVED_FROM ");
	if (ev->mask & IN_MOVED_TO)
		printf("IN_MOVED_TO ");

	if (ev->mask & IN_IGNORED)
		printf("IN_IGNORED ");
	if (ev->mask & IN_Q_OVERFLOW)
		printf("IN_Q_OVERFLOW ");
	if (ev->mask & IN_UNMOUNT)
		printf("IN_UNMOUNT ");

	if (ev->len > 0)
		printf("name = %s", ev->name);
	printf("\n");
}

int process_inotify_event(int fd, struct etcd_cdc_ctx *ctx,
			  char *iev_buf, int iev_len)
{
	struct inotify_event *ev;
	struct dir_watcher *tmp_watcher, *watcher = NULL;
	struct nvmet_subsys_host *host;
	struct nvmet_port_subsys *subsys;
	int ev_len;

	ev = (struct inotify_event *)iev_buf;
	display_inotify_event(ev);
	ev_len = sizeof(struct inotify_event) + ev->len;
	if (ev->mask & IN_IGNORED)
		return ev_len;

	list_for_each_entry(tmp_watcher, &dir_watcher_list, entry) {
		if (tmp_watcher->wd == ev->wd) {
			watcher = tmp_watcher;
			break;
		}
	}
	if (!watcher) {
		if (debug_inotify)
			printf("No watcher for wd %d\n", ev->wd);
		return ev_len;
	}
	if (ev->mask & IN_CREATE) {
		char subdir[FILENAME_MAX + 1];

		sprintf(subdir, "%s/%s", watcher->dirname, ev->name);
		if (debug_inotify) {
			if (ev->mask & IN_ISDIR)
				printf("mkdir %s\n", subdir);
			else
				printf("link %s\n", subdir);
		}
		switch (watcher->type) {
		case TYPE_HOST_DIR:
			watch_host(fd, ctx, watcher->dirname, ev->name);
			break;
		case TYPE_PORT_DIR:
			watch_port(fd, ctx, watcher->dirname, ev->name);
			break;
		case TYPE_PORT_SUBSYS_DIR:
			watch_port_subsys(fd, ctx, watcher->dirname, ev->name);
			break;
		case TYPE_SUBSYS_DIR:
			watch_subsys(fd, ctx, watcher->dirname, ev->name);
			break;
		case TYPE_SUBSYS_HOSTS_DIR:
			watch_subsys_host(fd, ctx, watcher->dirname, ev->name);
			break;
		default:
			fprintf(stderr, "Unhandled create type %d\n",
				watcher->type);
			break;
		}
	} else if (ev->mask & IN_DELETE_SELF) {
		struct nvmet_port *port;
		struct nvmet_subsys *subsys;
		struct nvmet_host *host;

		if (debug_inotify)
			printf("rmdir %s type %d\n",
			       watcher->dirname, watcher->type);

		/* Watcher is already removed */
		list_del_init(&watcher->entry);
		switch (watcher->type) {
		case TYPE_PORT:
			port = container_of(watcher,
					    struct nvmet_port, watcher);
			free(port);
			break;
		case TYPE_SUBSYS:
			subsys = container_of(watcher,
					      struct nvmet_subsys, watcher);
			free(subsys);
			break;
		case TYPE_HOST:
			host = container_of(watcher,
					    struct nvmet_host, watcher);
			free(host);
			break;
		default:
			free(watcher);
			break;
		}
	} else if (ev->mask & IN_DELETE) {
		char subdir[FILENAME_MAX + 1];

		sprintf(subdir, "%s/%s", watcher->dirname, ev->name);
		if (debug_inotify) {
			if (ev->mask & IN_ISDIR)
				printf("rmdir %s\n", subdir);
			else
				printf("unlink %s\n", subdir);
		}
		list_for_each_entry(tmp_watcher, &dir_watcher_list, entry) {
			if (strcmp(tmp_watcher->dirname, subdir))
				continue;
			watcher = tmp_watcher;
		}
		if (watcher) {
			remove_watch(fd, watcher);
			switch (watcher->type) {
			case TYPE_SUBSYS_HOST:
				host = container_of(watcher,
						    struct nvmet_subsys_host,
						    watcher);
				host->subsys = NULL;
				free(host);
				gen_disc_aen(ctx);
				break;
			case TYPE_PORT_SUBSYS:
				subsys = container_of(watcher,
						      struct nvmet_port_subsys,
						      watcher);
				subsys->port = NULL;
				free(subsys);
				gen_disc_aen(ctx);
				break;
			default:
				fprintf(stderr, "Unhandled delete type %d\n",
					watcher->type);
				free(watcher);
				break;
			}
		}
	} else if (ev->mask & IN_MODIFY) {
		struct nvmet_port_attr *port_attr;

		if (debug_inotify)
			printf("write %s %s\n", watcher->dirname, ev->name);

		switch (watcher->type) {
		case TYPE_SUBSYS:
			host = container_of(watcher,
					    struct nvmet_subsys_host,
					    watcher);
			gen_disc_aen(ctx);
			break;
		case TYPE_PORT_ATTR:
			port_attr = container_of(watcher,
						 struct nvmet_port_attr,
						 watcher);
			if (!port_attr->port) {
				fprintf(stderr, "No port link for %s %s\n",
					watcher->dirname, port_attr->attr);
			} else {
				port_read_attr(port_attr->port,
					       watcher->dirname,
					       port_attr->attr);
				gen_disc_aen(ctx);
			}
			break;
		default:
			fprintf(stderr, "unhandled modify type %d\n",
				watcher->type);
			free(watcher);
			break;
		}
	}
	return ev_len;
}

int watch_host_dir(int fd, struct etcd_cdc_ctx *ctx)
{
	char hosts_dir[PATH_MAX + 1];
	DIR *hd;
	struct dirent *he;

	strcpy(hosts_dir, ctx->configfs);
	strcat(hosts_dir, "/hosts");
	watch_directory(fd, hosts_dir, TYPE_HOST_DIR,
			IN_CREATE | IN_DELETE_SELF);

	hd = opendir(hosts_dir);
	if (!hd) {
		fprintf(stderr, "Cannot open %s\n", hosts_dir);
		return -1;
	}
	while ((he = readdir(hd))) {
		if (!strcmp(he->d_name, ".") ||
		    !strcmp(he->d_name, ".."))
			continue;
		watch_host(fd, ctx, hosts_dir, he->d_name);
	}
	closedir(hd);
	return 0;
}

int watch_port_dir(int fd, struct etcd_cdc_ctx *ctx)
{
	char ports_dir[PATH_MAX + 1];
	DIR *pd;
	struct dirent *pe;

	strcpy(ports_dir, ctx->configfs);
	strcat(ports_dir, "/ports");
	watch_directory(fd, ports_dir, TYPE_PORT_DIR,
			IN_CREATE | IN_DELETE_SELF);

	pd = opendir(ports_dir);
	if (!pd) {
		fprintf(stderr, "Cannot open %s\n", ports_dir);
		return -1;
	}
	while ((pe = readdir(pd))) {
		if (!strcmp(pe->d_name, ".") ||
		    !strcmp(pe->d_name, ".."))
			continue;
		watch_port(fd, ctx, ports_dir, pe->d_name);
	}
	closedir(pd);
	return 0;
}

int watch_subsys_dir(int fd, struct etcd_cdc_ctx *ctx)
{
	char subsys_dir[PATH_MAX + 1];
	DIR *sd;
	struct dirent *se;

	strcpy(subsys_dir, ctx->configfs);
	strcat(subsys_dir, "/subsystems");
	watch_directory(fd, subsys_dir, TYPE_SUBSYS_DIR,
			IN_CREATE | IN_DELETE_SELF);

	sd = opendir(subsys_dir);
	if (!sd) {
		fprintf(stderr, "Cannot open %s\n", subsys_dir);
		return -1;
	}
	while ((se = readdir(sd))) {
		if (!strcmp(se->d_name, ".") ||
		    !strcmp(se->d_name, ".."))
			continue;
		watch_subsys(fd, ctx, subsys_dir, se->d_name);
	}
	closedir(sd);
	return 0;
}

void cleanup_watcher(int fd)
{
	struct dir_watcher *watcher, *tmp_watch;

    	list_for_each_entry_safe(watcher, tmp_watch, &dir_watcher_list, entry) {
		remove_watch(fd, watcher);
		free(watcher);
	}
}

void inotify_loop(struct etcd_cdc_ctx *ctx,
		  int inotify_fd, int signal_fd)
{
	fd_set rfd;
	struct timeval tmo;
	char event_buffer[INOTIFY_BUFFER_SIZE]
		__attribute__ ((aligned(__alignof__(struct inotify_event))));

	watch_host_dir(inotify_fd, ctx);
	watch_subsys_dir(inotify_fd, ctx);
	watch_port_dir(inotify_fd, ctx);

	for (;;) {
		int rlen, ret;
		char *iev_buf;

		FD_ZERO(&rfd);
		FD_SET(signal_fd, &rfd);
		FD_SET(inotify_fd, &rfd);
		tmo.tv_sec = ctx->ttl / 5;
		tmo.tv_usec = 0;
		ret = select(inotify_fd + 1, &rfd, NULL, NULL, &tmo);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "select returned %d", errno);
			break;
		}
		if (ret == 0) {
			/* Select timeout*/
			continue;
		}
		if (!FD_ISSET(inotify_fd, &rfd)) {
			struct signalfd_siginfo fdsi;

			if (!FD_ISSET(signal_fd, &rfd)) {
				fprintf(stderr,
					"select returned for invalid fd");
				continue;
			}
			rlen = read(signal_fd, &fdsi, sizeof(fdsi));
			if (rlen != sizeof(fdsi)) {
				fprintf(stderr,
					"Couldn't read siginfo\n");
				exit(1);
			}
			if (fdsi.ssi_signo == SIGINT ||
			    fdsi.ssi_signo == SIGTERM) {
				fprintf(stderr,
					"signal %d received, terminating\n",
					fdsi.ssi_signo);
				break;
			}
		}
		rlen = read(inotify_fd, event_buffer, INOTIFY_BUFFER_SIZE);
		if (rlen < 0) {
			fprintf(stderr, "error %d on reading inotify event",
				errno);
			continue;
		}
		for (iev_buf = event_buffer;
		     iev_buf < event_buffer + rlen; ) {
			int iev_len;

			iev_len = process_inotify_event(inotify_fd, ctx,
							iev_buf,
							event_buffer + rlen - iev_buf);
			if (iev_len < 0) {
				fprintf(stderr, "Failed to process inotify\n");
				break;
			}
			iev_buf += iev_len;
		}
	}
	cleanup_watcher(inotify_fd);
}

