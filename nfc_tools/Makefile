# SPDX-License-Identifier: GPL-2.0-only
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Werror

.PHONY: all clean catalog

all: qti-nfc-function

catalog:
	python3 generate-function-catalog.py \
		../include/uapi/linux/nfc/qti_nfc_function.h \
		qti_nfc_function_catalog.inc

qti-nfc-function: qti_nfc_function.c qti_nfc_function_lib.c \
		qti_nfc_function_lib.h qti_nfc_function_catalog.inc
	$(CC) $(CPPFLAGS) $(CFLAGS) qti_nfc_function.c \
		qti_nfc_function_lib.c -o $@

clean:
	rm -f qti-nfc-function
