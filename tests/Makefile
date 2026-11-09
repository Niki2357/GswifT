# Unit tests for the pure Swift algorithm. Runs on macOS or Linux.
CC ?= cc
SAN = -fsanitize=undefined -fno-sanitize-recover=undefined
ifeq ($(shell uname),Linux)
SAN += -fsanitize=address   # ASan aborts at startup on recent macOS
endif
CFLAGS = -std=c11 -O1 -g -Wall -Wextra -Werror $(SAN)

.PHONY: test clean
test: test_swift_core
	./test_swift_core

test_swift_core: test_swift_core.c ../src/swift_core.h ../src/swift_types.h
	$(CC) $(CFLAGS) -o $@ test_swift_core.c

clean:
	rm -rf test_swift_core test_swift_core.dSYM
