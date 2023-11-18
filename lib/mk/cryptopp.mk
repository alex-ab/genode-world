CRYPTOPP_SRC_DIR = $(call select_from_ports,cryptopp)/src/lib/cryptopp

LIBS += stdcxx libm

CRYPTOPP_FILTER := adhoc.cpp test.cpp bench1.cpp bench2.cpp validat0.cpp validat1.cpp validat2.cpp validat3.cpp validat4.cpp datatest.cpp regtest1.cpp regtest2.cpp regtest3.cpp dlltest.cpp fipsalgt.cpp tweetnacl.cpp simon-simd.cpp simon.cpp speck-simd.cpp speck.cpp

CRYPTOPP_SRC := $(notdir $(wildcard $(CRYPTOPP_SRC_DIR)/*.cpp))

SRC_CC += $(filter-out $(CRYPTOPP_FILTER),$(CRYPTOPP_SRC))

CXX_DEF += -DCRYPTOPP_DISABLE_SSSE3

CC_CXX_OPT_STD = -std=gnu++17

ifneq ($(filter $(SPECS),arm arm_64),)
	CXX_DEF += -DCRYPTOPP_DISABLE_ASM
endif

CC_WARN += -Wno-delete-non-virtual-dtor

vpath %.cpp $(CRYPTOPP_SRC_DIR)

CC_CXX_WARN_STRICT =
