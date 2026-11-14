# Nova interpreter - build script (Linux, macOS, BSD and Windows via
# MSYS2/MinGW make, or use build_windows.bat).
#   cl /O2 /std:c11 /W4 /Fe:nova.exe src\*.c bcrypt.lib ws2_32.lib   (MSVC)

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter
SRC      = src/value.c src/gc.c src/lexer.c src/parser.c src/interp.c src/builtins.c src/web.c src/mail.c src/json.c src/crypto.c src/pkg.c src/main.c
HDR      = src/nova.h

# Windows (MinGW) needs bcrypt for BCryptGenRandom and ws2_32 for the
# SMTP/sockets in mail.c; POSIX systems link plain -lm.
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)
LDFLAGS  = -lm
ifneq (,$(findstring NT,$(UNAME_S)))
    LDFLAGS += -lbcrypt -lws2_32
endif

all: nova

nova: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o nova $(SRC) $(LDFLAGS)

debug:
	$(CC) -g -O0 -std=c11 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -o nova_debug $(SRC) $(LDFLAGS)

# Portable binary for cPanel / shared-host uploads. Note: glibc static
# builds may warn about getaddrinfo (DNS) — for production prefer building
# on the target server, or use musl (musl-gcc make static).
static:
	$(CC) -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -static -o nova $(SRC) $(LDFLAGS)

test: nova
	./tests/run_tests.sh

asan-test: debug
	NOVA=./nova_debug ./tests/run_tests.sh

clean:
	rm -f nova nova_debug

.PHONY: all debug static test asan-test clean
