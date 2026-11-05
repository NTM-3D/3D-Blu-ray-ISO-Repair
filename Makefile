VERSION := $(shell sed -n 's/^\#define BR3D_VER_MAJOR *//p' src/version.h).$(shell sed -n 's/^\#define BR3D_VER_MINOR *//p' src/version.h)

ifeq ($(OS),Windows_NT)
  BIN   := 3d_bluray_iso_repair_v$(VERSION).exe
  RCOBJ := src/version_rc.o
else
  BIN   := 3d_bluray_iso_repair
  RCOBJ :=
endif

RM := rm -f

CC     ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
SRCS   := src/udf.c src/udf_repair.c src/clpi.c src/repair.c src/report.c src/main.c
OBJS   := $(SRCS:.c=.o)

all: $(BIN)

$(BIN): $(OBJS) $(RCOBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(RCOBJ)

src/%.o: src/%.c src/udf.h src/clpi.h src/repair.h src/report.h src/version.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/version_rc.o: src/version.rc src/version.h
	windres src/version.rc -O coff -o $@

clean:
	-$(RM) $(OBJS) $(RCOBJ) $(BIN) 3d_bluray_iso_repair.exe

.PHONY: all clean