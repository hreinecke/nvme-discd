#ifndef _DISCDB_H
#define _DISCDB_H

int discdb_init(void);
int discdb_exit(void);
int discdb_open(const char *filename);
void discdb_close(const char *filename);

int discdb_add_host(struct nvmet_host *host);
int discdb_del_host(struct nvmet_host *host);
int discdb_add_subsys(struct nvmet_subsys *subsys);
int discdb_modify_subsys(struct nvmet_subsys *subsys);
int discdb_del_subsys(struct nvmet_subsys *subsys);
int discdb_add_port(struct nvmet_port *port, u8 subtype);
int discdb_modify_port(struct nvmet_port *port, char *attr);
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
