# Nova interpreter - build script (Linux, macOS, BSD)
# Windows: use build_windows.bat (MinGW/MSVC) or:
#   gcc -O2 -std=c11 -Wall -Wextra -o nova.exe src\*.c -lm

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter
SRC      = src/value.c src/gc.c src/lexer.c src/parser.c src/interp.c src/builtins.c src/web.c src/mail.c src/json.c src/crypto.c src/pkg.c src/main.c
HDR      = src/nova.h

all: nova

nova: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o nova $(SRC) -lm

debug:
	$(CC) -g -O0 -std=c11 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -o nova_debug $(SRC) -lm

# Portable binary for cPanel / shared-host uploads. Note: glibc static
# builds may warn about getaddrinfo (DNS) — for production prefer building
# on the target server, or use musl (musl-gcc make static).
static:
	$(CC) -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -static -o nova $(SRC) -lm

test: nova
	./tests/run_tests.sh

asan-test: debug
	NOVA=./nova_debug ./tests/run_tests.sh

clean:
	rm -f nova nova_debug

.PHONY: all debug static test asan-test clean
