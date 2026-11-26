# Builds smrun against a libmgba build (see tools/build_mgba.sh).
#   make -C tools/mgba MGBA=/path/to/mgba MGBA_BUILD=/path/to/mgba/build
MGBA ?= ../../third_party/mgba
MGBA_BUILD ?= $(MGBA)/build
CFLAGS ?= -O2 -g
smrun: smrun.c
	$(CC) $(CFLAGS) -I$(MGBA)/include -I$(MGBA_BUILD)/include -DBUILD_STATIC -DENABLE_DEBUGGERS -DENABLE_DIRECTORIES -DENABLE_GDB_STUB -DENABLE_VFS -DENABLE_VFS_FD -DHAVE_CRC32 -DHAVE_LOCALE -DHAVE_PTHREAD_CREATE -DHAVE_STRDUP -DHAVE_STRLCPY -DHAVE_STRNDUP -DM_CORE_GBA -DUSE_PNG -DUSE_PTHREADS -DUSE_ZLIB -D_GNU_SOURCE $< -o $@ \
	  $(MGBA_BUILD)/libmgba.a -lpng -lz -lm -lpthread
clean:
	rm -f smrun
