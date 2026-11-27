CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?= -static
ALL_CFLAGS = -std=c11 -D_GNU_SOURCE $(CFLAGS)

SRC = $(wildcard src/*.c src/vendor/*.c)
LIB = $(filter-out src/ui.c,$(SRC))

bedrock: $(SRC) src/bedrock.h $(wildcard src/vendor/*.h)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(SRC) -lpthread

tests/run: tests/test.c $(LIB) src/bedrock.h
	$(CC) $(ALL_CFLAGS) -Isrc -o $@ tests/test.c $(LIB) -lpthread

test: bedrock tests/run
	cd tests && ./run

# docs/sample-certificate.json: signed with the fixed sample key, for checking the verify page
sample: tests/run
	cd tests && ./run --sample > ../../docs/sample-certificate.json

clean:
	rm -f bedrock tests/run

.PHONY: test sample clean
