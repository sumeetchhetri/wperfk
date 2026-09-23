CFLAGS  += -std=c99 -Wall -O2 -D_REENTRANT
LIBS    := -lm -lssl -lcrypto -lpthread

TARGET  := $(shell uname -s | tr '[A-Z]' '[a-z]' 2>/dev/null || echo unknown)

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
endif

SRC  := wrk.c net.c ssl.c aprintf.c stats.c script.c units.c \
		ae.c zmalloc.c http_parser.c tinymt64.c hdr_histogram.c
BIN  := wperfk

ODIR := obj
OBJ  := $(patsubst %.c,$(ODIR)/%.o,$(SRC)) $(ODIR)/bytecode.o

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

ifneq ($(WITH_OPENSSL),)
	CFLAGS  += -I$(WITH_OPENSSL)/include
	LDFLAGS += -L$(WITH_OPENSSL)/lib
endif

all: $(BIN)

clean:
	$(RM) -r $(BIN) $(ODIR)
	@$(MAKE) -C $(LUAJIT_DIR) clean

$(BIN): $(OBJ)
	@echo LINK $(BIN)
	@$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(OBJ): config.h Makefile $(LUAJIT_DEP) | $(ODIR)

$(ODIR):
	@mkdir -p $@

# Emit bytecode as C (not a host object file) so cross builds work.
$(ODIR)/bytecode.c: src/wrk.lua $(LUAJIT_DEP) | $(ODIR)
	@echo LUAJIT $<
	@$(SHELL) -c '$(LUAJIT_BC)'

$(ODIR)/bytecode.o: $(ODIR)/bytecode.c
	@echo CC $<
	@$(CC) $(CFLAGS) -c -o $@ $<

$(ODIR)/%.o : %.c
	@echo CC $<
	@$(CC) $(CFLAGS) -c -o $@ $<

$(LUAJIT_DIR)/libluajit.a:
	@echo Building LuaJIT...
	@test -f $(LUAJIT_DIR)/Makefile || { echo "deps/LuaJIT missing: run git submodule update --init"; exit 1; }
	@$(MAKE) -C $(LUAJIT_DIR) BUILDMODE=static

.PHONY: all clean
.SUFFIXES:
.SUFFIXES: .c .o .lua

vpath %.c   src
vpath %.h   src
vpath %.lua scripts
