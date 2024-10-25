#ifndef _DISCDB_H
#define _DISCDB_H

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

int discdb_init(void);
int discdb_exit(void);
int discdb_open(const char *filename);
void discdb_close(const char *filename);

int discdb_add_host(struct nvmet_host *host);
int discdb_del_host(struct nvmet_host *host);
int discdb_add_subsys(struct nvmet_subsys *subsys);
int discdb_modify_subsys(struct nvmet_subsys *subsys);
int discdb_del_subsys(struct nvmet_subsys *subsys);
int discdb_add_port(struct nvmet_port *port);
int discdb_del_port(struct nvmet_port *port);
int discdb_add_host_subsys(struct nvmet_host *host,
			   struct nvmet_subsys *subsys);
int discdb_del_host_subsys(struct nvmet_host *host,
			   struct nvmet_subsys *subsys);
int discdb_add_subsys_port(struct nvmet_subsys *subsys,
			   struct nvmet_port *port);
int discdb_del_subsys_port(struct nvmet_subsys *subsys,
			   struct nvmet_port *port);

int discdb_host_disc_entries(const char *hostnqn, u8 *log, int log_len);
int discdb_host_genctr(const char *hostnqn);

#endif
