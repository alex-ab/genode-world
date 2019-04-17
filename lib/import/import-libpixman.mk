PIXMAN_PORT_DIR := $(call select_from_ports,libpixman)
INC_DIR += $(PIXMAN_PORT_DIR)/include/libpixman $(call select_from_repositories,include/libpixman)
