CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?= -static
ALL_CFLAGS = -std=c11 -D_GNU_SOURCE $(CFLAGS)

SRC = $(wildcard src/*.c) src/vendor/qrcodegen.c
LIB = $(filter-out src/ui.c,$(SRC))

bedrock: $(SRC) src/bedrock.h
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(SRC) -lpthread

tests/run: tests/test.c $(LIB) src/bedrock.h
	$(CC) $(ALL_CFLAGS) -Isrc -o $@ tests/test.c $(LIB) -lpthread

test: bedrock tests/run
	cd tests && ./run

clean:
	rm -f bedrock tests/run

.PHONY: test clean
