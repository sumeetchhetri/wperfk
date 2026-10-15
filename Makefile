CFLAGS  += -std=c99 -Wall -O2 -D_REENTRANT -Ideps/llhttp
ODIR    := obj
ifneq ($(VERSION),)
	CFLAGS += -DVERSION='"$(VERSION)"'
endif
LIBS     = -lm $(SSL_LIBS) -lpthread

TARGET  := $(shell uname -s | tr '[A-Z]' '[a-z]' 2>/dev/null || echo unknown)
# Windows: native MSYS2 (MINGW64/UCRT64/CLANGARM64) or a MinGW cross compiler.
ifneq ($(filter %-mingw32 %-windows-gnu,$(shell $(CC) -dumpmachine 2>/dev/null)),)
	TARGET := windows
endif

ifeq ($(TARGET), sunos)
	CFLAGS += -D_PTHREADS -D_POSIX_C_SOURCE=200112L
	LIBS   += -lsocket
else ifeq ($(TARGET), darwin)
	# LuaJIT forces 10.4 if unset; 11.0 is the arm64 minimum.
	export MACOSX_DEPLOYMENT_TARGET ?= 11.0
	WITH_OPENSSL ?= $(shell brew --prefix openssl@3 2>/dev/null)
else ifeq ($(TARGET), linux)
	CFLAGS  += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
	LIBS    += -ldl
	LDFLAGS += -Wl,-E
else ifeq ($(TARGET), freebsd)
	CFLAGS  += -D_DECLARE_C99_LDBL_MATH
	LDFLAGS += -Wl,-E
	# LuaJIT defaults to CC=gcc; FreeBSD ships only cc (clang).
	LUAJIT_MAKEFLAGS ?= CC=$(CC)
else ifeq ($(TARGET), windows)
	# gnu99: MinGW hides POSIX names (fdopen, dup, ...) under strict C99.
	CFLAGS  := $(filter-out -std=c99,$(CFLAGS)) -std=gnu99
	CFLAGS  += -D_WIN32_WINNT=0x0601 -DWIN32_LEAN_AND_MEAN -DNOGDI -DNOMINMAX
	CFLAGS  += -Isrc/compat/win32 -Ideps/wepoll
	LIBS    += -lws2_32 -lcrypt32 -lbcrypt -luser32 -ladvapi32 -lgdi32
	# Native MSYS2: don't rely on LuaJIT parsing `uname -s` (which varies by
	# MSYS2 environment). Cross builds set LUAJIT_MAKEFLAGS themselves.
	ifeq ($(OS),Windows_NT)
		LUAJIT_MAKEFLAGS ?= HOST_SYS=Windows HOST_MSYS=mingw TARGET_SYS=Windows
	endif
	WIN_SRC := compat_win32.c
	WIN_OBJ := $(ODIR)/wepoll.o
	EXE     := .exe
endif

# STATIC=1: fully static on Linux (use musl, e.g. Alpine); on macOS link
# OpenSSL statically (libSystem is always dynamic there).
SSL_LIBS := -lssl -lcrypto
ifeq ($(STATIC),1)
	ifeq ($(TARGET), darwin)
		SSL_LIBS := $(WITH_OPENSSL)/lib/libssl.a $(WITH_OPENSSL)/lib/libcrypto.a
	else
		LDFLAGS  += -static
	endif
endif

SRC  := wrk.c net.c ssl.c aprintf.c stats.c script.c units.c \
		ae.c zmalloc.c url.c tinymt64.c hdr_histogram.c prometheus.c $(WIN_SRC)
BIN  := wperfk$(EXE)

LLHTTP_OBJ := $(ODIR)/llhttp_api.o $(ODIR)/llhttp_http.o $(ODIR)/llhttp.o
OBJ  := $(patsubst %.c,$(ODIR)/%.o,$(SRC)) $(ODIR)/bytecode.o $(LLHTTP_OBJ) $(WIN_OBJ)

LUAJIT_DIR := deps/LuaJIT/src

ifneq ($(WITH_LUAJIT),)
	CFLAGS     += -I$(WITH_LUAJIT)/include/luajit-2.1
	LDFLAGS    += -L$(WITH_LUAJIT)/lib
	LIBS       := -lluajit-5.1 $(LIBS)
	LUAJIT_DEP :=
	LUAJIT_BC   = $(WITH_LUAJIT)/bin/luajit -b $(CURDIR)/$< $(CURDIR)/$@
else
	CFLAGS     += -I$(LUAJIT_DIR)
	LIBS       := $(LUAJIT_DIR)/libluajit.a $(LIBS)
	LUAJIT_DEP := $(LUAJIT_DIR)/libluajit.a
	LUAJIT_BC   = cd $(LUAJIT_DIR) && ./luajit -b $(CURDIR)/$< $(CURDIR)/$@
endif

# Cross builds can't run the target luajit: point HOST_LUAJIT at a host
# LuaJIT 2.1 (same pointer size) and pass the cross settings to LuaJIT, e.g.
#   make CC=x86_64-w64-mingw32-gcc HOST_LUAJIT=/usr/bin/luajit \
#        LUAJIT_MAKEFLAGS="CC=gcc HOST_CC=gcc CROSS=x86_64-w64-mingw32- TARGET_SYS=Windows"
ifneq ($(HOST_LUAJIT),)
	# cd: an uninstalled luajit finds its jit.* modules relative to cwd
	LUAJIT_BC   = cd $(dir $(abspath $(HOST_LUAJIT))) && $(abspath $(HOST_LUAJIT)) -b $(CURDIR)/$< $(CURDIR)/$@
endif

ifneq ($(WITH_OPENSSL),)
	CFLAGS  += -I$(WITH_OPENSSL)/include
	LDFLAGS += -L$(WITH_OPENSSL)/lib
endif

all: $(BIN)

clean:
	$(RM) -r wperfk wperfk.exe $(ODIR)
	@$(MAKE) -C $(LUAJIT_DIR) clean

$(BIN): $(OBJ)
	@echo LINK $(BIN)
	@$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

# Every object depends on every header: struct layouts are shared across files.
$(OBJ): config.h Makefile $(LUAJIT_DEP) $(wildcard src/*.h) | $(ODIR)

$(ODIR):
	@mkdir -p $@

# Emit bytecode as C (not a host object file) so cross builds work.
$(ODIR)/bytecode.c: src/wrk.lua $(LUAJIT_DEP) | $(ODIR)
	@echo LUAJIT $<
	@$(SHELL) -c '$(LUAJIT_BC)'
	@printf 'const unsigned long luaJIT_BC_wrk_size = sizeof(luaJIT_BC_wrk);\n' >> $@

$(ODIR)/bytecode.o: $(ODIR)/bytecode.c
	@echo CC $<
	@$(CC) $(CFLAGS) -c -o $@ $<

$(ODIR)/%.o : %.c
	@echo CC $<
	@$(CC) $(CFLAGS) -c -o $@ $<

# Vendored third-party code: build quietly.
$(ODIR)/wepoll.o: deps/wepoll/wepoll.c | $(ODIR)
	@echo CC $<
	@$(CC) $(CFLAGS) -w -c -o $@ $<

$(ODIR)/llhttp_%.o: deps/llhttp/%.c deps/llhttp/llhttp.h | $(ODIR)
	@echo CC $<
	@$(CC) $(CFLAGS) -w -c -o $@ $<

$(ODIR)/llhttp.o: deps/llhttp/llhttp.c deps/llhttp/llhttp.h | $(ODIR)
	@echo CC $<
	@$(CC) $(CFLAGS) -w -c -o $@ $<

$(LUAJIT_DIR)/libluajit.a:
	@echo Building LuaJIT...
	@test -f $(LUAJIT_DIR)/Makefile || { echo "deps/LuaJIT missing: run git submodule update --init"; exit 1; }
	@$(MAKE) -C $(LUAJIT_DIR) BUILDMODE=static $(LUAJIT_MAKEFLAGS)

.PHONY: all clean
.SUFFIXES:
.SUFFIXES: .c .o .lua

vpath %.c   src
vpath %.h   src
vpath %.lua scripts
