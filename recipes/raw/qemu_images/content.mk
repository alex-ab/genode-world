content: image-hw-log.elf

image-hw-log.elf:
	cp $(GENODE_DIR)/build/arm_v8a/var/run/log/boot/image-hw.elf $@
