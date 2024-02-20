CPP=gcc
OPTS=-g -Wall
LIBS=-lresolv -ldl -lm

# Modify SRC_DIR as necessary
SRC_DIR=$(HOME)/postgresql-16.1

INCLUDE=-I$(SRC_DIR)/src/include     

freelist_lru.o: freelist_lru.c
	$(CPP) $(OPTS) $(INCLUDE) -c -o freelist_lru.o freelist_lru.c

freelist_elru.o: freelist_elru.c
	$(CPP) $(OPTS) $(INCLUDE) -c -o freelist_elru.o freelist_elru.c

clean:
	rm -f *.o

elru: copyelru pgsql

lru: copylru pgsql

clock: copyclock pgsql

copyelru:
	cp freelist_elru.c $(SRC_DIR)/src/backend/storage/buffer/freelist.c
	cp bufmgr_elru.c $(SRC_DIR)/src/backend/storage/buffer/bufmgr.c

copylru:
	cp freelist_lru.c $(SRC_DIR)/src/backend/storage/buffer/freelist.c
	cp bufmgr_lru.c $(SRC_DIR)/src/backend/storage/buffer/bufmgr.c

copyclock:
	cp freelist.original.c $(SRC_DIR)/src/backend/storage/buffer/freelist.c
	cp bufmgr.original.c $(SRC_DIR)/src/backend/storage/buffer/bufmgr.c

pgsql:
	cd $(SRC_DIR) && make && make install

