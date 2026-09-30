# Compile check (no link): every runtime source against test/port/.
# Run through tools/check.sh.
.SUFFIXES:
include $(DEVKITPRO)/devkitARM/base_rules
NX    := $(DEVKITPRO)/libnx32
B     := /work/test/build
ARCH  := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mfloat-abi=softfp \
         -mfpu=neon-fp-armv8 -mtp=soft -fPIE -ftls-model=local-exec
CFLAGS := -g -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
          -ffunction-sections -fdata-sections $(ARCH) -D__SWITCH__ \
          -I/work/test/port -I/work/source -I$(B) -I$(NX)/include -I/portlibs/include \
          -DDCR_GL_MESA=$(GL) -DPORT_PAYLOAD_NAME=\"dcrsea_nx\" \
          -Werror=implicit-function-declaration -Werror=implicit-int \
          -Werror=int-conversion -Werror=incompatible-pointer-types -Werror=return-type
ASFLAGS := -g $(ARCH) -I/work/test/port -I/work/source -DPORT_PAYLOAD_NAME=\"dcrsea_nx\"
GL ?= 1
ifeq ($(strip $(LIST)),)
FILES := $(notdir $(wildcard /work/source/*.c) $(wildcard /work/source/*.S))
else
FILES := $(LIST)
endif
OBJS := $(addprefix $(B)/,$(addsuffix .o,$(basename $(FILES))))
all: $(OBJS)
	@echo "check: $(words $(OBJS)) file(s) compiled"
$(B):
	@mkdir -p $@
$(B)/dcr_build.h: | $(B)
	@echo '#define DCR_BUILD 202601010000ULL' > $@
$(B)/%.o: /work/source/%.c $(B)/dcr_build.h FORCE | $(B)
	@echo $(notdir $<)
	@$(CC) $(CFLAGS) -c $< -o $@
$(B)/%.o: /work/source/%.S FORCE | $(B)
	@echo $(notdir $<)
	@$(CC) $(ASFLAGS) -c $< -o $@
FORCE:
.PHONY: all FORCE
