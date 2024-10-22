#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sqlite3.h>
#include <errno.h>

#include "common.h"
#include "discdb.h"

static sqlite3 *nvme_db;

static int sql_exec_simple(const char *sql_str)
{
	int ret;
	char *errmsg = NULL;

	ret = sqlite3_exec(nvme_db, sql_str, NULL, NULL, &errmsg);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "SQL error executing %s\n", sql_str);
		fprintf(stderr, "SQL error: %s\n", errmsg);
		sqlite3_free(errmsg);
	}
	return ret;
}

static const char *init_sql[5] = {
"CREATE TABLE host ( id INTEGER PRIMARY KEY AUTOINCREMENT, "
"nqn VARCHAR(223) UNIQUE NOT NULL);",
"CREATE TABLE subsys ( id INTEGER PRIMARY KEY AUTOINCREMENT, "
"nqn VARCHAR(223) UNIQUE NOT NULL, allow_any INT DEFAULT 1);",
"CREATE TABLE port ( portid INT NOT NULL PRIMARY KEY,"
"trtype INT DEFAULT 3, adrfam INT DEFAULT 1, subtype INT DEFAULT 2, "
"treq INT DEFAULT 0, traddr CHAR(255) NOT NULL, "
"trsvcid CHAR(32) DEFAULT '', tsas CHAR(255) DEFAULT '');",
"CREATE TABLE host_subsys ( host_id INT NOT NULL, subsys_id INT NOT NULL, "
"FOREIGN KEY (host_id) REFERENCES host(id) "
"ON UPDATE CASCADE ON DELETE RESTRICT, "
"FOREIGN KEY (subsys_id) REFERENCES subsys(id) "
"ON UPDATE CASCADE ON DELETE RESTRICT);",
"CREATE TABLE subsys_port ( subsys_id INT NOT NULL, port_id INT NOT NULL, "
"FOREIGN KEY (subsys_id) REFERENCES subsys(id) "
"ON UPDATE CASCADE ON DELETE RESTRICT, "
"FOREIGN KEY (port_id) REFERENCES port(portid) "
"ON UPDATE CASCADE ON DELETE RESTRICT);",
};

int discdb_init(void)
{
	int i, ret;

	for (i = 0; i < 5; i++) {
		ret = sql_exec_simple(init_sql[i]);
	}
	return ret;
}

static const char *exit_sql[5] =
{
	"DROP TABLE subsys_port;",
	"DROP TABLE host_subsys;",
	"DROP TABLE port;",
	"DROP TABLE subsys;",
	"DROP TABLE host;",
};

int discdb_exit(void)
{
	int i, ret;

	for (i = 0; i < 5; i++) {
		ret = sql_exec_simple(exit_sql[i]);
	}
	return ret;
}

static char add_host_sql[] =
	"INSERT INTO host (nqn) VALUES ('%s');";

int discdb_add_host(struct nvmet_host *host)
{
	char *sql;
	int ret;

	ret = asprintf(&sql, add_host_sql, host->hostnqn);
	if (ret < 0)
		return ret;
	ret = sql_exec_simple(sql);
	free(sql);
	return ret;
}

static char del_host_sql[] =
	"DELETE FROM host (nqn) SELECT nqn FROM host WHERE nqn LIKE '%s';";

int discdb_del_host(struct nvmet_host *host)
{
	char *sql;
	int ret;

	ret = asprintf(&sql, del_host_sql, host->hostnqn);
	if (ret < 0)
		return ret;
	ret = sql_exec_simple(sql);
	free(sql);
	return ret;
}

static char add_subsys_sql[] =
	"INSERT INTO subsys (nqn) VALUES ('%s');";

int discdb_add_subsys(struct nvmet_subsys *subsys)
{
	char *sql;
	int ret;

	ret = asprintf(&sql, add_subsys_sql, subsys->subsysnqn);
	if (ret < 0)
		return ret;
	ret = sql_exec_simple(sql);
	free(sql);
	return ret;
}

static char del_subsys_sql[] =
	"DELETE FROM subsys (nqn) SELECT nqn FROM subsys WHERE nqn LIKE '%s';";

int discdb_del_subsys(struct nvmet_subsys *subsys)
{
	char *sql;
	int ret;

	ret = asprintf(&sql, del_subsys_sql, subsys->subsysnqn);
	if (ret < 0)
		return ret;
	ret = sql_exec_simple(sql);
	free(sql);
	return ret;
}

static char add_port_sql[] =
	"INSERT INTO port (portid, trtype, adrfam, treq, traddr, trsvcid, tsas)"
	" VALUES ('%d','%s','%s','%s','%s','%s','%s');";

int discdb_add_port(struct nvmet_port *port)
{
	char *sql;
	int ret;

	ret = asprintf(&sql, add_port_sql, port->port_id, port->trtype,
		       port->adrfam, port->treq, port->traddr, port->trsvcid,
		       port->tsas);
	if (ret < 0)
		return ret;
	ret = sql_exec_simple(sql);
	free(sql);
	return ret;
}

static char del_port_sql[] =
	"DELETE FROM port SELECT port_id FROM port WHERE port_id = '%d';";

int discdb_del_port(struct nvmet_port *port)
{
	char *sql;
	int ret;

	ret = asprintf(&sql, del_port_sql, port->port_id);
	if (ret < 0)
		return ret;
	ret = sql_exec_simple(sql);
	free(sql);
	return ret;
}

static char add_host_subsys_sql[] =
	"INSERT INTO host_subsys (host_id, subsys_id) "
	"SELECT host.id, subsys.id FROM host, subsys "
	"WHERE host.nqn LIKE '%s' AND subsys.nqn LIKE '%s';";

int discdb_add_host_subsys(struct nvmet_host *host, struct nvmet_subsys *subsys)
{
	char *sql;
	int ret;

	ret = asprintf(&sql, add_host_subsys_sql,
		       host->hostnqn, subsys->subsysnqn);
	if (ret < 0)
		return ret;
	ret = sql_exec_simple(sql);
	free(sql);
	return ret;
}

static char del_host_subsys_sql[] =
	"DELETE FROM host_subsys "
	"SELECT host.id, subsys.id FROM host, subsys "
	"WHERE host.nqn LIKE '%s' AND subsys.nqn LIKE '%s';";

int discdb_del_host_subsys(struct nvmet_host *host, struct nvmet_subsys *subsys)
{
	char *sql;
	int ret;

	ret = asprintf(&sql, del_host_subsys_sql,
		       host->hostnqn, subsys->subsysnqn);
	if (ret < 0)
		return ret;
	ret = sql_exec_simple(sql);
	free(sql);
	return ret;
}

static char add_subsys_port_sql[] =
	"INSERT INTO subsys_port (subsys_id, port_id) "
	"SELECT subsys.id, port.portid FROM subsys, port "
	"WHERE subsys.nqn LIKE '%s' AND port.portid = %d;";

int discdb_add_subsys_port(struct nvmet_subsys *subsys, struct nvmet_port *port)
{
	char *sql;
	int ret;

	ret = asprintf(&sql, add_subsys_port_sql,
		       subsys->subsysnqn, port->port_id);
	if (ret < 0)
		return ret;
	ret = sql_exec_simple(sql);
	free(sql);
	return ret;
}

static char del_subsys_port_sql[] =
	"DELETE FROM subsys_port "
	"SELECT subsys.id, port.portid FROM subsys, port, subsys_port"
	"WHERE subsys.nqn LIKE '%s' AND port.portid = %d AND "
	"subsys_port.subsys_id = subsys.id AND subsys_port.portid = port.portid;";

int discdb_del_subsys_port(struct nvmet_subsys *subsys, struct nvmet_port *port)
{
	char *sql;
	int ret;

	ret = asprintf(&sql, del_subsys_port_sql,
		       subsys->subsysnqn, port->port_id);
	if (ret < 0)
		return ret;
	ret = sql_exec_simple(sql);
	free(sql);
	return ret;
}

int discdb_open(const char *filename)
{
	int ret;

	ret = sqlite3_open(filename, &nvme_db);
	if (ret) {
		fprintf(stderr, "Can't open database: %sj\n",
			sqlite3_errmsg(nvme_db));
		sqlite3_close(nvme_db);
		return -ENOENT;
	}
	discdb_init();
	return 0;
}

void discdb_close(const char *filename)
{
	discdb_exit();
	sqlite3_close(nvme_db);
	unlink(filename);
}
