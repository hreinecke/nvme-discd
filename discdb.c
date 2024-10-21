#include <stdio.h>
#include <sqlite3.h>
#include <errno.h>

#include "discdb.h"

static sqlite3 *nvme_db;

static const char *init_sql[7] = {
"CREATE DATABASE nvme_discdb CHARACTER SET ascii;",
"USE nvme_discdb;",
"CREATE TABLE host ( id INT NOT NULL AUTO_INCREMENT, PRIMARY KEY (id), "
	"nqn CHAR(223) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL);",
"CREATE TABLE subsys ( id INT NOT NULL AUTO_INCREMENT, PRIMARY KEY (id), "
	"nqn CHAR(223) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,);"
"CREATE TABLE port ( portid INT NOT NULL, PRIMARY KEY (portid),"
	"trtype INT DEFAULT 3, adrfam INT DEFAULT 1, subtype INT DEFAULT 2, "
	"treq INT DEFAULT 0, traddr CHAR(255) NOT NULL, "
	"trsvcid CHAR(32) DEFAULT '', tsas CHAR(255) DEFAULT '');"
"CREATE TABLE host_subsys ( id INT NOT NULL AUTO_INCREMENT, "
	"host_id INT NOT NULL, subsys_id INT NOT NULL, "
	"PRIMARY KEY (id), INDEX host (host_id), INDEX subsys (subsys_id), "
	"FOREIGN KEY (host_id) REFERENCES host(id) "
	"ON UPDATE CASCADE ON DELETE RESTRICT, "
	"FOREIGN KEY (subsys_id) REFERENCES subsys(id) "
	"ON UPDATE CASCADE ON DELETE RESTRICT);"
"CREATE TABLE subsys_port ( id INT NOT NULL AUTO_INCREMENT, "
	"subsys_id INT NOT NULL, port_id INT NOT NULL, "
	"PRIMARY KEY (id), INDEX subsys (subsys_id), INDEX port (port_id), "
	"FOREIGN KEY (subsys_id) REFERENCES subsys(id) "
	"ON UPDATE CASCADE ON DELETE RESTRICT, "
	"FOREIGN KEY (port_id) REFERENCES port(portid) "
	"ON UPDATE CASCADE ON DELETE RESTRICT);"
};

int discdb_init(void)
{
	int i, ret;
	char *errmsg = NULL;

	for (i = 0; i < 7; i++) {
		ret = sqlite3_exec(nvme_db, init_sql[i], NULL, NULL, &errmsg);
		if (ret != SQLITE_OK) {
			fprintf(stderr, "SQL error executing %s\n",
				init_sql[i]);
			fprintf(stderr, "SQL error: %s\n", errmsg);
			sqlite3_free(errmsg);
		}
	}
	return ret;
}

static const char drop_table[] =
	"DROP TABLE subsys_port, port_subsys, port, subsys, host;";
static const char drop_database[] =
	"DROP DATABASE nvme_discdb;";

int discdb_exit(void)
{
	int ret;
	char *errmsg = NULL;

	ret = sqlite3_exec(nvme_db, drop_table, NULL, NULL, &errmsg);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "SQL error executing %s\n",
			drop_table);
		fprintf(stderr, "SQL error: %s\n", errmsg);
		sqlite3_free(errmsg);
	}
	ret = sqlite3_exec(nvme_db, drop_database, NULL, NULL, &errmsg);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "SQL error executing %s\n",
			drop_table);
		fprintf(stderr, "SQL error: %s\n", errmsg);
		sqlite3_free(errmsg);
	}
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
	return 0;
}

void discdb_close(void)
{
	sqlite3_close(nvme_db);
}
