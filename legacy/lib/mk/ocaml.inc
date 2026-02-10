OCAML_PORT_DIR := $(call select_from_ports,ocaml)
OCAML_SRC_DIR :=  $(OCAML_PORT_DIR)/src/ocaml

INC_DIR += $(REP_DIR)/src/app/ocamlrun

# workaround for https://codeberg.org/genodelabs/genode-world/issues/180
REQUIRES += x86_64
