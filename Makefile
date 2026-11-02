CC      ?= cc
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE
LDFLAGS ?=
LDLIBS  := -pthread

SRC := src/main.c src/util.c src/scan.c src/index.c src/snapshot.c src/query.c src/fsreader_ext4.c src/httpd.c src/live.c src/perm.c src/fsops.c
OBJ := $(SRC:.c=.o)
BIN := bin/xuanlan

PREFIX ?= /usr/local

all: $(BIN)

$(BIN): $(OBJ) | bin
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

src/%.o: src/%.c src/xl.h src/version.h | bin
	$(CC) $(CFLAGS) -c -o $@ $<

src/version.h:
	@printf '#ifndef XL_APP_VERSION\n#define XL_APP_VERSION "dev"\n#endif\n' > $@

bin:
	mkdir -p bin

clean:
	rm -f $(OBJ) $(BIN)

test: $(BIN)
	./tests/smoke.sh
	./tests/ext4_image_test.sh

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/xuanlan

.PHONY: all clean install test
