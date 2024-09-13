LIBGO_SUPPORT_SRC_DIR := $(REP_DIR)/src/lib/libgo_support

SRC_S  := getcontext.S setcontext.S swapcontext.S
#SRC_C  := makecontext.c setcontexthelper.c

#LIBS := base libc

SHARED_LIB = yes

CC_CXX_WARN_STRICT =

vpath %.cc $(LIBGO_SUPPORT_SRC_DIR)
vpath %.S  $(LIBGO_SUPPORT_SRC_DIR)/spec/$(SPEC_ARCH)
vpath %.c  $(LIBGO_SUPPORT_SRC_DIR)/spec/$(SPEC_ARCH)
