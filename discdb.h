#ifndef _DISCDB_H
#define _DISCDB_H

int discdb_init(void);
int discdb_exit(void);
int discdb_open(const char *filename);
void discdb_close(void);

#endif
