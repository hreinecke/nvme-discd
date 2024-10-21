#include <stdio.h>
#include <unistd.h>
#include <sqlite3.h>
#include <errno.h>

#include "discdb.h"

static sqlite3 *nvme_db;

static const char *init_sql[5] = {
"CREATE TABLE host ( id INT NOT NULL PRIMARY KEY, "
"nqn VARCHAR(223) UNIQUE NOT NULL);",
"CREATE TABLE subsys ( id INT NOT NULL PRIMARY KEY, "
"nqn VARCHAR(223) UNIQUE NOT NULL);",
"CREATE TABLE port ( portid INT NOT NULL PRIMARY KEY,"
"trtype INT DEFAULT 3, adrfam INT DEFAULT 1, subtype INT DEFAULT 2, "
"treq INT DEFAULT 0, traddr CHAR(255) NOT NULL, "
"trsvcid CHAR(32) DEFAULT '', tsas CHAR(255) DEFAULT '');",
"CREATE TABLE host_subsys ( id INT NOT NULL PRIMARY KEY, "
"host_id INT NOT NULL, subsys_id INT NOT NULL, "
"FOREIGN KEY (host_id) REFERENCES host(id) "
"ON UPDATE CASCADE ON DELETE RESTRICT, "
"FOREIGN KEY (subsys_id) REFERENCES subsys(id) "
"ON UPDATE CASCADE ON DELETE RESTRICT);",
"CREATE TABLE subsys_port ( id INT NOT NULL PRIMARY KEY, "
"subsys_id INT NOT NULL, port_id INT NOT NULL, "
"FOREIGN KEY (subsys_id) REFERENCES subsys(id) "
"ON UPDATE CASCADE ON DELETE RESTRICT, "
"FOREIGN KEY (port_id) REFERENCES port(portid) "
"ON UPDATE CASCADE ON DELETE RESTRICT);",
};

int discdb_init(void)
{
	int i, ret;
	char *errmsg = NULL;

	for (i = 0; i < 5; i++) {
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
	char *errmsg = NULL;

	for (i = 0; i < 5; i++) {
		ret = sqlite3_exec(nvme_db, exit_sql[i], NULL, NULL, &errmsg);
		if (ret != SQLITE_OK) {
			fprintf(stderr, "SQL error executing %s\n",
				exit_sql[i]);
			fprintf(stderr, "SQL error: %s\n", errmsg);
			sqlite3_free(errmsg);
		}
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
	discdb_init();
	return 0;
}

void discdb_close(const char *filename)
{
	discdb_exit();
	sqlite3_close(nvme_db);
	unlink(filename);
}
