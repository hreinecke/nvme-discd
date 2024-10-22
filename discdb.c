#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sqlite3.h>
#include <errno.h>

#include "common.h"
#include "discdb.h"

static sqlite3 *nvme_db;

static int sql_simple_cb(void *unused, int argc, char **argv, char **colname)
{
	   int i;

	   for (i = 0; i < argc; i++) {
		   printf("%s ", colname[i]);
	   }
	   printf("\n");
	   for (i = 0; i < argc; i++) {
		   printf("%s ",
			  argv[i] ? argv[i] : "NULL");
	   }
	   printf("\n");
	   return 0;
}

static int sql_exec_simple(const char *sql_str)
{
	int ret;
	char *errmsg = NULL;

	ret = sqlite3_exec(nvme_db, sql_str, sql_simple_cb, NULL, &errmsg);
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
"CREATE TABLE host_subsys ( host_id INTEGER, subsys_id INTEGER, "
"FOREIGN KEY (host_id) REFERENCES host(id) "
"ON UPDATE CASCADE ON DELETE RESTRICT, "
"FOREIGN KEY (subsys_id) REFERENCES subsys(id) "
"ON UPDATE CASCADE ON DELETE RESTRICT);",
"CREATE TABLE subsys_port ( subsys_id INTEGER, port_id INTEGER, "
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
	"DELETE FROM host WHERE nqn LIKE '%s';";

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
	"DELETE FROM subsys WHERE nqn LIKE '%s';";

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
	"DELETE FROM port WHERE portid = '%d';";

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
	printf("Contents of 'host_subsys':\n");
	ret = sql_exec_simple("SELECT host.nqn AS host_nqn, subsys.nqn AS subsys_nqn FROM host_subsys INNER JOIN subsys ON subsys.id = host_subsys.subsys_id INNER JOIN host ON host.id = host_subsys.host_id;");
	if (ret)
		return ret;
	return ret;
}

static char del_host_subsys_sql[] =
	"DELETE FROM host_subsys "
	"WHERE host_subsys.host_id in "
	"(SELECT id FROM host WHERE nqn LIKE '%s') AND "
	"(SELECT id FROM subsys WHERE nqn LIKE '%s');";

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
	"VALUES (%d, %d);";

static int sql_value_cb(void *val, int argc, char **argv, char **colname)
{
	int i, retval;

	for (i = 0; i < argc; i++) {
		char *eptr = NULL;

		if (!strcmp(colname[i], "id")) {
			retval = strtoul(argv[i], &eptr, 10);
			if (argv[i] != eptr)
				*(int *)val = retval;
		}
	}
	return 0;
}

static char select_subsys_port_sql[] =
	"SELECT subsys.nqn, subsys_port.port_id "
	"FROM subsys_port "
	"INNER JOIN subsys ON subsys.id = subsys_port.subsys_id;";

int discdb_add_subsys_port(struct nvmet_subsys *subsys, struct nvmet_port *port)
{
	char *sql, *errmsg = NULL;
	int ret, val;

	ret = asprintf(&sql, "SELECT id FROM subsys WHERE nqn LIKE '%s';",
		       subsys->subsysnqn);
	if (ret < 0)
		return ret;

	ret = sqlite3_exec(nvme_db, sql, sql_value_cb, &val, &errmsg);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "SQL error executing %s\n", sql);
		fprintf(stderr, "SQL error: %s\n", errmsg);
		sqlite3_free(errmsg);
	}
	free(sql);

	ret = asprintf(&sql, add_subsys_port_sql,
		       val, port->port_id);
	if (ret < 0)
		return ret;
	ret = sql_exec_simple(sql);
	free(sql);
	printf("Contents of 'subsys_port':\n");
	ret = sql_exec_simple(select_subsys_port_sql);
	if (ret)
		return ret;
	return ret;
}

static char del_subsys_port_sql[] =
	"DELETE FROM subsys_port "
	"WHERE subsys_port.subsys_id in ("
	"SELECT id FROM subsys WHERE nqn LIKE '%s') AND "
	"subsys_port.port_id IN ("
	"SELECT portid FROM port WHERE portid = %d);";

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

static char host_disc_entry_sql[] =
	"SELECT host.nqn AS host_nqn, subsys.nqn AS subsys_nqn, subsys_port.port_id AS portid "
	"FROM subsys_port "
	"INNER JOIN subsys ON subsys.id = subsys_port.subsys_id "
	"INNER JOIN host_subsys ON host_subsys.subsys_id = subsys_port.subsys_id "
	"INNER JOIN host ON host_subsys.host_id = host.id AND host.nqn LIKE '%s';";

int discdb_host_disc_entries(struct nvmet_host *host)
{
	char *sql;
	int ret;

	printf("Display disc entries for %s\n", host->hostnqn);
	ret = asprintf(&sql, host_disc_entry_sql, host->hostnqn);
	if (ret < 0)
		return ret;
	ret = sql_exec_simple(sql);
	free(sql);
	return ret;
}

static char subsys_disc_entry_sql[] =
	"SELECT host.nqn AS host_nqn, subsys.nqn AS subsys_nqn, subsys_port.port_id AS portid "
	"FROM subsys_port "
	"INNER JOIN subsys ON subsys.id = subsys_port.subsys_id AND subsys.nqn LIKE '%s' "
	"INNER JOIN host_subsys ON host_subsys.subsys_id = subsys_port.subsys_id "
	"INNER JOIN host ON host_subsys.host_id = host.id;";

int discdb_subsys_disc_entries(struct nvmet_subsys *subsys)
{
	char *sql;
	int ret;

	printf("Display disc entries for %s\n", subsys->subsysnqn);
	ret = asprintf(&sql, subsys_disc_entry_sql, subsys->subsysnqn);
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
