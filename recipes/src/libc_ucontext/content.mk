MIRROR_FROM_REP_DIR := lib/mk/libc_ucontext.inc \
                       lib/mk/spec/x86_64/libc_ucontext.mk \
                       lib/mk/spec/arm_64/libc_ucontext.mk 

SRC_DIR = src/lib/libgo_support

context: $(SRC_DIR) LICENSE $(MIRROR_FROM_REP_DIR)

# XXX mirror only context files, not all XXX
$(SRC_DIR):
	mkdir -p $@
	cp -rH $(REP_DIR)/$@/* $@/
#src/lib/libgo_support:
#	mkdir -p $(dir $@)
#	cp -r $(PORT_DIR)/src/lib/libgo_support $@

#include:
#	mkdir -p $@
#	cp -a $(PORT_DIR)/include/* $@


$(MIRROR_FROM_REP_DIR):
	$(mirror_from_rep_dir)

LICENSE:
	touch $@
#	cp $(PORT_DIR)/src/lib/libuvc/LICENSE.txt $@
