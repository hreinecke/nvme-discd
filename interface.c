#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "common.h"
#include "discdb.h"

int interface_from_port(struct etcd_cdc_ctx *ctx,
			struct nvmet_port *port)
{
	struct addrinfo hints, *result, *rp;
	int ret;

	printf("Create interface for port %d: %s %s\n",
	       port->port_id, port->traddr, port->trsvcid);
	if (!strcmp(port->trtype, "tcp"))
		return -EINVAL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_family = AF_UNSPEC;
	hints.ai_flags = AI_PASSIVE;

	ret = getaddrinfo(port->traddr, port->trsvcid, &hints, &result);
	if (ret != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
		return -EHOSTUNREACH;
	}

	for (rp = result; rp != NULL; rp = rp->ai_next) {
		char abuf[INET6_ADDRSTRLEN];
		const char *addr;
		in_port_t port;

		if (rp->ai_family == AF_INET) {
			struct sockaddr_in *sinp;

			sinp = (struct sockaddr_in *)rp->ai_addr;
			addr = inet_ntop(rp->ai_family, &sinp->sin_addr,
					 abuf, INET_ADDRSTRLEN);
			port = ntohs(sinp->sin_port);
		} else {
			struct sockaddr_in6 *sinp;

			sinp = (struct sockaddr_in6 *)rp->ai_addr;
			addr = inet_ntop(rp->ai_family, &sinp->sin6_addr,
					 abuf, INET6_ADDRSTRLEN);
			port = ntohs(sinp->sin6_port);
		}
		printf("%s: %s addr %s:%d\n", __func__,
		       rp->ai_family == AF_INET ? "ipv4" : "ipv6", addr, port);
	}
	return 0;
}

