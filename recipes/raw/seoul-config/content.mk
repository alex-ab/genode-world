CONFIG_FILES = vm_seoul.cfg vm_seoul_boot_0.cfg vm_seoul_boot_1.cfg
CONFIG_FILES += vm_seoul_audio.cfg

content: $(CONFIG_FILES)

$(CONFIG_FILES):
	cp $(REP_DIR)/recipes/raw/seoul-config/$@ $@
