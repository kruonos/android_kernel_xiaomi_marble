# SPDX-License-Identifier: GPL-2.0-only
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Werror

.PHONY: all clean

all: qca-bt-dump

qca-bt-dump: qca_bt_dump.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

clean:
	rm -f qca-bt-dump
