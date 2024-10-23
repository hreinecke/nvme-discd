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
"nqn VARCHAR(223) UNIQUE NOT NULL, genctr INTEGER DEFAULT 0);",
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

static char select_host_subsys_sql[] =
	"SELECT h.nqn AS host_nqn, s.nqn AS subsys_nqn "
	"FROM host_subsys AS hs "
	"INNER JOIN subsys AS s ON s.id = hs.subsys_id "
	"INNER JOIN host AS h ON h.id = hs.host_id;";

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
	ret = sql_exec_simple(select_host_subsys_sql);
	if (ret)
		return ret;
	ret = asprintf(&sql, "UPDATE host SET genctr = genctr + 1 "
		       "WHERE nqn LIKE '%s';", host->hostnqn);
	if (ret < 0)
		return ret;
	ret = sql_exec_simple(sql);
	free(sql);
	return ret;
}

static char del_host_subsys_sql[] =
	"DELETE FROM host_subsys AS hs "
	"WHERE hs.host_id IN "
	"(SELECT id FROM host WHERE nqn LIKE '%s') AND "
	"hs.subsys_id IN "
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
	"SELECT subsys.id, port.portid FROM subsys, port "
	"WHERE subsys.nqn LIKE '%s' AND port.portid = '%d';";

static char select_subsys_port_sql[] =
	"SELECT s.nqn, p.portid, p.trtype, p.traddr "
	"FROM subsys_port AS sp "
	"INNER JOIN subsys AS s ON s.id = sp.subsys_id "
	"INNER JOIN port AS p ON p.portid = sp.port_id;";

static char update_genctr_host_subsys_sql[] =
	"UPDATE host SET genctr = genctr + 1 "
	"FROM "
	"(SELECT s.nqn AS subsys_nqn, hs.host_id AS host_id "
	"FROM host_subsys AS hs "
	"INNER JOIN subsys AS s ON s.id = hs.subsys_id) AS hs "
	"WHERE hs.host_id = host.id AND hs.subsys_nqn LIKE '%s';";

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
	printf("Contents of 'subsys_port':\n");
	ret = sql_exec_simple(select_subsys_port_sql);

	ret = asprintf(&sql, update_genctr_host_subsys_sql,
		       subsys->subsysnqn);
	if (ret < 0)
		return ret;

	ret = sql_exec_simple(sql);
	free(sql);

	return ret;
}

static char del_subsys_port_sql[] =
	"DELETE FROM subsys_port AS sp "
	"WHERE sp.subsys_id in "
	"(SELECT id FROM subsys WHERE nqn LIKE '%s') AND "
	"sp.port_id IN "
	"(SELECT portid FROM port WHERE portid = %d);";

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

	ret = asprintf(&sql, update_genctr_host_subsys_sql,
		       subsys->subsysnqn);
	if (ret < 0)
		return ret;

	ret = sql_exec_simple(sql);
	free(sql);

	return ret;
}

static char host_disc_entry_sql[] =
	"SELECT h.nqn AS host_nqn, h.genctr, s.nqn AS subsys_nqn, "
	"p.portid, p.trtype, p.traddr, p.trsvcid, p.treq, p.tsas "
	"FROM subsys_port AS sp "
	"INNER JOIN subsys AS s ON s.id = sp.subsys_id "
	"INNER JOIN host_subsys AS hs ON hs.subsys_id = sp.subsys_id "
	"INNER JOIN host AS h ON hs.host_id = h.id "
	"INNER JOIN port AS p ON sp.port_id = p.portid "
	"WHERE h.nqn LIKE '%s';";

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
	"SELECT h.nqn AS host_nqn, h.genctr, s.nqn AS subsys_nqn, "
	"p.portid, p.trtype, p.traddr, p.trsvcid, p.treq, p.tsas "
	"FROM subsys_port AS sp "
	"INNER JOIN subsys AS s ON s.id = sp.subsys_id "
	"INNER JOIN host_subsys AS hs ON hs.subsys_id = sp.subsys_id "
	"INNER JOIN host AS h ON hs.host_id = h.id "
	"INNER JOIN port AS p ON p.portid = sp.port_id "
	"WHERE s.nqn LIKE '%s';";

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
