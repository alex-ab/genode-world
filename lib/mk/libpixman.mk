SHARED_LIB =1

LIBS += libc

CC_DEF := -DHAVE_CONFIG_H \
          -DPIXMAN_NO_TLS

CC_WARN := -Wno-unused-const-variable

CC_OPT_pixman-ssse3 := -mssse3

PIXMAN_PORT_DIR = $(call select_from_ports,libpixman)

PIXMAN_SRC_DIR = $(PIXMAN_PORT_DIR)/src/lib/libpixman

INC_DIR += $(REP_DIR)/src/lib/libpixman \
           $(PIXMAN_SRC_DIR)/pixman

PIXMAN_FILTER := pixman-region.c pixman-vmx.c

PIXMAN_SRC := $(notdir $(wildcard $(PIXMAN_SRC_DIR)/pixman/*.c))

SRC_C := $(filter-out $(PIXMAN_FILTER),$(PIXMAN_SRC))

vpath %.c $(PIXMAN_SRC_DIR)/pixman
