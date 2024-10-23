#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "common.h"
#include "discdb.h"

LIST_HEAD(interface_list);

static int portid;

int interface_create(struct etcd_cdc_ctx *ctx,
		     struct nvmet_port *port)
{
	struct interface *iface;

	iface = malloc(sizeof(struct interface));
	if (!iface)
		return -1;
	memset(iface, 0, sizeof(struct interface));
	INIT_LIST_HEAD(&iface->node);
	iface->ctx = ctx;
	iface->port = port;
	iface->portid = portid++;
	pthread_mutex_init(&iface->ep_mutex, NULL);
	printf("%s: %s addr %s:%d\n", __func__,
	       port->adrfam, port->traddr, ctx->port);

	if (!strcmp(port->adrfam, "ipv4")) {
		struct sockaddr_in *addr = (struct sockaddr_in *)&iface->addr;

		memset(addr, 0, sizeof(*addr));
		addr->sin_family = AF_INET;
		addr->sin_port = htons(ctx->port);
		inet_pton(AF_INET, port->traddr, &addr->sin_addr);
	} else {
		struct sockaddr_in6 *addr6 = &iface->addr;

		memset(addr6, 0, sizeof(*addr6));
		addr6->sin6_family = AF_INET6;
		addr6->sin6_port = htons(ctx->port);
		inet_pton(AF_INET6, port->traddr, &addr6->sin6_addr);
	}
	list_add(&iface->node, &interface_list);
	return 0;
}

int interface_delete(struct etcd_cdc_ctx *ctx, struct nvmet_port *port)
{
	struct interface *iface, *tmp;

	list_for_each_entry(tmp, &interface_list, node) {
		if (tmp->port == port) {
			iface = tmp;
			break;
		}
	}
	if (iface) {
		list_del_init(&iface->node);
		printf("%s: %s addr %s:%d\n", __func__,
		       port->adrfam, port->traddr, ctx->port);
		free(iface);
	}
	return 0;
}
