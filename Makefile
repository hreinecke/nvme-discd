
PRG = nvme_discd
PRG_OBJS = daemon.o inotify.o discdb.o interface.o tcp.o endpoint.o cmds.o
CFLAGS = -Wall -g
LIBS = -lsqlite3 -lpthread

all:	$(PRG)

$(PRG): $(PRG_OBJS)
	$(CC) $(CFLAGS) -o $(PRG) $^ $(LIBS)

$(DISC): $(DISC_OBJS) $(B64)
	$(CC) $(CFLAGS) -o $(DISC) $^ $(LIBS) -lpthread

$(TEST): $(TEST_OBJS) $(B64)
	$(CC) $(CFLAGS) -o $(TEST) $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $?

clean:
	$(RM) $(TEST_OBJS) $(PRG_OBJS) $(DISC_OBJS) $(PRG) $(TEST) $(DISC)

daemon.c: common.h list.h discdb.h
inotify.c: list.h discdb.h
discdb.c: common.h discdb.h
interface: common.h discdb.h tcp.h
tcp.c: common.h tcp.h
endpoint.c: common.h endpoint.h
