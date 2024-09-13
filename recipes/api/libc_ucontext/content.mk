content: lib/symbols/libc_ucontext LICENSE

#PORT_DIR := $(call port_dir,$(REP_DIR)/ports/libconfig)

#include:
#	mkdir -p $@
#	cp -r $(PORT_DIR)/include/libconfig.h $@

lib/symbols/libc_ucontext:
	$(mirror_from_rep_dir)

LICENSE:
	touch $@
