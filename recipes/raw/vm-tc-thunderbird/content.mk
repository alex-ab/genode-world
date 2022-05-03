CONFIG_FILES = tc-thunderbird.raw

content: $(CONFIG_FILES)

$(CONFIG_FILES):
	cp $(REP_DIR)/recipes/raw/vm-tc-thunderbird/$@ $@
