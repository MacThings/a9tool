CC = cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
ifeq ($(shell uname -s),Darwin)
# Always universal and compatible down to macOS Monterey 12.0.
override CFLAGS += -arch arm64 -arch x86_64 -mmacosx-version-min=12.0
endif
SRC = src/a9tool.c src/a9_lzss.c src/a9_md5.c src/a9_csv_validator.c src/csvp_schema.c src/a9_compare.c src/a9_port.c
HEADERS = src/a9_table.h src/a9_md5.h src/a9_lzss.h src/a9_csv_validator.h src/csvp_schema.h src/a9_compare.h src/a9_port.h
all: a9tool
a9tool: $(SRC) $(HEADERS)
	$(CC) $(CFLAGS) $(SRC) -o $@
check: a9tool
	mkdir -p build
	$(CC) $(CFLAGS) -Isrc tests/md5_known.c src/a9_md5.c -o build/md5_known
	./build/md5_known
	./tests/roundtrip.sh
	./tests/csv_validation.sh
	./tests/compare.sh
clean:
	rm -f a9tool
.PHONY: all check clean
