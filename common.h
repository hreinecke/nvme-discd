#ifndef __COMMON_H__
#define __COMMON_H__

#define unlikely __glibc_unlikely

#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <uuid/uuid.h>

#include "types.h"
#include "list.h"
#include "nvme.h"
#include "nvme_tcp.h"

#define NVMF_UUID_FMT		"nqn.2014-08.org.nvmexpress:uuid:%s"

#define NVMF_DQ_DEPTH		2
#define NVMF_SQ_DEPTH		128
#define NVMF_NUM_QUEUES		8

#define MAX_NQN_SIZE		256
#define MAX_ALIAS_SIZE		64

#define PAGE_SIZE		4096

#define KATO_INTERVAL	1000	/* in ms as per spec */
#define RETRY_COUNT	120	/* 2 min; value is multiplied with kato interval */

#define IPV4_LEN		4
#define IPV4_OFFSET		4
#define IPV4_DELIM		"."

#define IPV6_LEN		8
#define IPV6_OFFSET		8
#define IPV6_DELIM		":"

enum { DISCONNECTED, CONNECTED };

extern int stopped;

struct nvmet_host {
	char hostnqn[256];
};

struct nvmet_port {
	int port_id;
	char trtype[256];
	char traddr[256];
	char trsvcid[256];
	char adrfam[256];
	char treq[256];
	char tsas[256];
};

struct nvmet_subsys {
	char subsysnqn[256];
	int allow_any;
};

struct ep_qe {
	struct list_head node;
	int tag;
	struct endpoint *ep;
	struct nsdev *ns;
	union nvme_tcp_pdu pdu;
	struct iovec iovec;
	struct nvme_completion resp;
	void *data;
	u64 data_len;
	u64 data_pos;
	u64 data_remaining;
	u64 iovec_offset;
	int ccid;
	int opcode;
	bool busy;
};

enum { RECV_PDU, RECV_DATA, HANDLE_PDU };

struct endpoint {
	struct list_head node;
	pthread_t pthread;
	struct interface *iface;
	struct ctrl_conn *ctrl;
	struct ep_qe *qes;
	union nvme_tcp_pdu *recv_pdu;
	int recv_pdu_len;
	union nvme_tcp_pdu *send_pdu;
	int recv_state;
	int qsize;
	int state;
	int qid;
	int kato_countdown;
	int kato_interval;
	int sockfd;
	int maxr2t;
	int maxh2cdata;
	int mdts;
};

struct ctrl_conn {
	struct list_head node;
	char nqn[MAX_NQN_SIZE + 1];
	int cntlid;
	int ctrl_type;
	int kato;
	int num_endpoints;
	int max_endpoints;
	int aen_mask;
	u64 csts;
	u64 cc;
};

struct interface {
	struct list_head node;
	pthread_t pthread;
	struct etcd_cdc_ctx *ctx;
	struct list_head ep_list;
	pthread_mutex_t ep_mutex;
	struct nvmet_port port;
	sa_family_t adrfam;
	int portid;
	int listenfd;
	unsigned char *tls_key;
	size_t tls_key_len;
};

struct etcd_cdc_ctx {
	char *proto;
	int port;
	char *configfs;
	char *dbfile;
	int ttl;
	int debug;
	int tls;
	struct nvmet_host host;
	struct nvmet_subsys subsys;
};

extern int tcp_debug;
extern int cmd_debug;

static inline void set_response(struct nvme_completion *resp,
				__u16 ccid, __u16 status, bool dnr)
{
	if (!status)
		dnr = false;
	resp->command_id = ccid;
	resp->status = ((dnr ? NVME_STATUS_DNR : 0) | status) << 1;
}

void handle_disconnect(struct endpoint *ep, int shutdown);
int handle_request(struct endpoint *ep, struct nvme_command *cmd);
int handle_data(struct endpoint *ep, struct ep_qe *qe, int res);
int endpoint_update_qdepth(struct endpoint *ep, int qsize);

int interface_create(struct etcd_cdc_ctx *ctx, char *trtype,
		     char *traddr, char *adrfam);
void interface_delete(struct etcd_cdc_ctx *ctx, char *trtype,
		      char *traddr, char *adrfam);

#endif
