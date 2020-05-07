TARGET = adb
REQUIRES = x86

ADB_CONTRIB_DIR = $(call select_from_ports,adb)/src/app/android.core

# as specified in Android.bp 
CC_CXX_OPT += -Wno-unused-parameter
CC_CXX_OPT += -DADB_HOST=1
CC_CXX_OPT += -DALLOW_ADBD_ROOT=0
CC_CXX_OPT += -DANDROID_BASE_UNIQUE_FD_DISABLE_IMPLICIT_CONVERSION=1

#avoid warning about ignored attributes
CC_CXX_OPT += -Wno-attributes
#avoid warning about clang specific pragmas
CC_CXX_OPT += -Wno-unknown-pragmas

#CC_CXX_OPT += -Wno-effc++

LIBS += libc posix stdcxx libssl

SRC_CC = adb/client/main.cpp

INC_DIR += $(ADB_CONTRIB_DIR)/base/include
INC_DIR += $(ADB_CONTRIB_DIR)/libcutils/include
INC_DIR += $(ADB_CONTRIB_DIR)/adb

vpath %.cpp $(ADB_CONTRIB_DIR)

#LIBS   += base blit seoul_libc_support
#SRC_CC  = component.cc user_env.cc device_model_registry.cc state.cc
#SRC_CC += console.cc keyboard.cc network.cc disk.cc
#SRC_BIN = mono.tff

#MODEL_SRC_CC    += $(notdir $(wildcard $(SEOUL_CONTRIB_DIR)/model/*.cc))
#EXECUTOR_SRC_CC += $(notdir $(wildcard $(SEOUL_CONTRIB_DIR)/executor/*.cc))

#ifneq ($(filter x86_64, $(SPECS)),)
#CC_CXX_OPT += -mcmodel=large
#endif

#SRC_CC += $(filter-out $(FILTER_OUT),$(addprefix model/,$(MODEL_SRC_CC)))
#SRC_CC += $(filter-out $(FILTER_OUT),$(addprefix executor/,$(EXECUTOR_SRC_CC)))

#INC_DIR += $(SEOUL_CONTRIB_DIR)/include
#INC_DIR += $(SEOUL_GENODE_DIR)/include
#INC_DIR += $(REP_DIR)/src/app/seoul/include
#include $(call select_from_repositories,lib/mk/libc-common.inc)

#CC_CXX_WARN_STRICT = -Wextra -Weffc++ -Werror
#CC_WARN += -Wno-parentheses -Wall -Wno-unused

# XXX fix the warnings and remove this option
#CC_WARN += -Wno-error=implicit-fallthrough

#vpath %.cc  $(ADB_CONTRIB_DIR)
#vpath %.cc  $(REP_DIR)/src/app/seoul
#vpath %.tff $(REP_DIR)/src/app/seoul
