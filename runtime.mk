#---------------------------------------------------------------------------------
# runtime.mk -- the build every android32 port shares. A port's Makefile sets a
# few variables, includes this, then adds its own rules:
#
#   TARGET               := labyrinth2_nx        (required: the 32-bit program)
#   PORT_NPDM_PROGRAM_ID := 0x0100000000001010   (required: the title's id)
#   include runtime/runtime.mk
#   $(BUILD)/lab_audio.o: CFLAGS += -Wno-sign-compare ...
#
# Built from runtime/source/ and the port's source/: a port file with the
# name of a runtime file (util.c, kuser.S) replaces it, so a port migrates one
# file at a time. The port's source/ comes first on the include path, so the
# runtime's rt_settings.h finds the port's port_config.h. (A runtime .c that
# includes "x.h" with quotes still gets the runtime's own x.h, from its own
# folder: override a header's users, not the header.)
#
# Output: $(TARGET).nsp -- an ExeFS NSP (main + main.npdm, 32-bit), the
# program the launcher NRO installs as the ExeFS override of its forwarder,
# or run in an emulator directly (a 32-bit program cannot be an NRO:
# hbloader is 64-bit) -- and $(TARGET).build, its build number.
#
# Builds with devkitARM + libnx32 inside the toolchain container: the port's
# ./build.sh runs runtime/tools/docker_build.sh, which runs make there.
#
# What a port may set (before the include, unless marked):
#   TARGET                 the program's name (also PORT_PAYLOAD_NAME in C,
#                          the NPDM's name, the romfs stamp's name)
#   BUILD, SOURCES         build and source folders (build, source)
#   PORT_NPDM_PROGRAM_ID   0x01000000000010xx: one per port
#   PORT_NPDM_VERSION      0.0.3 (flappy 0.1.0)
#   PORT_NPDM_MAIN_STACK   0x800000 (flappy 0x400000)
#   PORT_NPDM_ADDRSPACE    0 (a8r 2)
#   PORT_NPDM_NAME         $(TARGET)
#   PORT_NPDM_JSON         a whole NPDM json of the port's own, instead of the
#                          one made from runtime/npdm.json.in
#   RT_EXCLUDE             runtime files not to build (opensles.c, or opensles)
#   PORT_CFLAGS, PORT_ASFLAGS, PORT_LDFLAGS, PORT_LIBS
#                          added to the compile / link (after the include,
#                          CFLAGS += ... works as well)
#   PORT_BUILD_H_USERS     port files that include dcr_build.h (a8r_setup)
#   PORT_STAMP             more of what, when it changes, rebuilds everything
#                          (sonic: -$(DCR_VIDEO))
#   PORT_CLEAN             more files for make clean to remove
#   PORTLIBS               mesa32's lib/ + include/ (./portlibs32)
#   DCR_GL_MESA            1 = mesa/nouveau, 0 = the null renderer (default:
#                          mesa when $(PORTLIBS)/lib/libEGL.a is there)
#   PORT_MESA              26 = Mesa 26.2, 20 = Mesa 20.1 (default: whichever
#                          is in portlibs32)
#   CRT0_EXTRA             more flags for crt0_reloc.c (see its rule)
# After the include: per-file rules ($(BUILD)/x.o: CFLAGS += ...; an explicit
# rule for $(BUILD)/x.o replaces the pattern rule), more prerequisites.
# MIT.
#---------------------------------------------------------------------------------
# (first: MAKEFILE_LIST's last word is this file only until the next include)
RT_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
RT_SRC := $(RT_DIR)/source

.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO is not set. Build with ./build.sh (the toolchain container).")
endif
include $(DEVKITPRO)/devkitARM/base_rules

ifeq ($(strip $(TARGET)),)
$(error "The port's Makefile sets TARGET (the program's name) before including runtime.mk")
endif
BUILD   ?= build
SOURCES ?= source
NX      := $(DEVKITPRO)/libnx32

# softfp is not a preference, it is the ABI: armeabi-v7a passes float/double in
# core registers, and so do libnx32 and newlib (built soft-float). Building the
# host softfp makes every shim, callback and engine entry point agree without
# per-function pcs("aapcs") annotations, while still emitting VFP/NEON code.
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mfloat-abi=softfp \
        -mfpu=neon-fp-armv8 -mtp=soft -fPIE -ftls-model=local-exec

# Renderer: 1 = mesa/nouveau from portlibs32/ (mesa32's lib/ and include/, from
# its ./build.sh or its release tarball), 0 = the null renderer. Default: mesa
# when its libraries are present.
PORTLIBS ?= $(CURDIR)/portlibs32
ifeq ($(origin DCR_GL_MESA),undefined)
DCR_GL_MESA := $(if $(wildcard $(PORTLIBS)/lib/libEGL.a),1,0)
endif
# Which Mesa portlibs32 holds: 26 (Mesa 26.2, with its own Horizon backend,
# libnouveau_horizon.a) or 20 (Mesa 20.1 with libdrm_nouveau). Both work; the
# link line and gl_mesa.c follow it (RT_MESA).
ifeq ($(origin PORT_MESA),undefined)
PORT_MESA := $(if $(wildcard $(PORTLIBS)/lib/libnouveau_horizon.a),26,20)
endif

CFLAGS := -g -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
          -ffunction-sections -fdata-sections $(ARCH) -D__SWITCH__ \
          -I$(NX)/include -I$(SOURCES) -I$(RT_SRC) -I$(BUILD) -I$(PORTLIBS)/include \
          -DDCR_GL_MESA=$(DCR_GL_MESA) -DRT_MESA=$(PORT_MESA) -DPORT_PAYLOAD_NAME=\"$(TARGET)\" \
          -Werror=implicit-function-declaration -Werror=implicit-int \
          -Werror=int-conversion -Werror=incompatible-pointer-types \
          -Werror=return-type $(PORT_CFLAGS)
ASFLAGS := -g $(ARCH) -I$(SOURCES) -I$(RT_SRC) -I$(BUILD) \
           -DDCR_GL_MESA=$(DCR_GL_MESA) -DPORT_PAYLOAD_NAME=\"$(TARGET)\" $(PORT_ASFLAGS)

# dcr32.specs/.ld: -z notext + a page-0 relocator. See source/crt0_reloc.c.
# --wrap: libnx's own svcSetThreadCoreMask calls through dcr_sched.c's; newlib's
# printf core through bionic_printf.c's (a NULL %s is safe, as on bionic);
# with mesa, nouveau's buffer allocation through gl_mesa.c's. Each flag ships
# with its __wrap_ symbol in the runtime: the flag alone fails the link.
RT_SPECS ?= $(abspath $(RT_DIR)/dcr32.specs)
RT_LDSCRIPT ?= $(abspath $(RT_DIR)/dcr32.ld)
LDFLAGS := -specs=$(RT_SPECS) -T $(RT_LDSCRIPT) $(ARCH) -g \
           -Wl,-Map,$(BUILD)/$(TARGET).map -Wl,--no-enum-size-warning \
           -Wl,--wrap=svcSetThreadCoreMask -Wl,--wrap=_svfprintf_r -Wl,--wrap=_vfprintf_r
ifeq ($(DCR_GL_MESA),1)
LDFLAGS += -Wl,--wrap=nouveau_bo_new
ifeq ($(PORT_MESA),26)
# GLES 1 is its own library in 26.2 (its functions weak, so it links next to
# GLES 2); Mesa's util libraries, expat and zlib come with it.
GL_LIBS := -L$(PORTLIBS)/lib -lEGL -lGLESv2 -lGLESv1_CM -lglapi -lmesa_util_c11 -lblake3 \
           -lmesa_util -lmesa_util_simd -lexpat -lz -lstdc++
else
GL_LIBS := -L$(PORTLIBS)/lib -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lstdc++
endif
endif
LDFLAGS += $(PORT_LDFLAGS)
LIBS    := $(PORT_LIBS) $(GL_LIBS) -L$(NX)/lib -lminiz -lnx -lm

# The files: the port's, then the runtime's that the port neither replaces
# (a file of the same name, .c or .S) nor excludes (RT_EXCLUDE).
PORT_FILES := $(notdir $(wildcard $(SOURCES)/*.c $(SOURCES)/*.S))
RT_ALL     := $(notdir $(wildcard $(RT_SRC)/*.c $(RT_SRC)/*.S))
RT_FILES   := $(strip $(foreach f,$(RT_ALL),$(if $(filter $(basename $f),$(basename $(PORT_FILES) $(RT_EXCLUDE))),,$f)))
RT_REPLACED := $(strip $(foreach f,$(RT_ALL),$(if $(filter $(basename $f),$(basename $(PORT_FILES))),$f)))
OFILES := $(addprefix $(BUILD)/,$(addsuffix .o,$(basename $(PORT_FILES)))) \
          $(addprefix $(BUILD)/rt/,$(addsuffix .o,$(basename $(RT_FILES))))
DEPS   := $(OFILES:.o=.d)

.DEFAULT_GOAL := all
.PHONY: all clean rt-files FORCE
all: $(TARGET).nsp $(TARGET).build

# make rt-files: what is built from where.
rt-files:
	@echo "port files:       $(PORT_FILES)"
	@echo "runtime files:    $(RT_FILES)"
	@echo "replaced by port: $(RT_REPLACED)"
	@echo "excluded:         $(RT_EXCLUDE)"
	@echo "renderer:         DCR_GL_MESA=$(DCR_GL_MESA)"

$(BUILD) $(BUILD)/rt:
	@mkdir -p $@

# Switching renderers changes the compile flags: rebuild everything.
RENDERER_STAMP := $(BUILD)/.renderer-$(DCR_GL_MESA)-mesa$(PORT_MESA)$(PORT_STAMP)
$(RENDERER_STAMP): | $(BUILD)
	@rm -f $(BUILD)/.renderer-*
	@touch $@

# The build number (UTC minutes): compiled in (DCR_BUILD: setup, config) and
# written next to the NSP as $(TARGET).build. The launcher NRO carries both;
# the wrapper updates its ExeFS override from an NRO only when the NRO's
# number is higher. Rewritten only when the minute changed; made before any
# compile (order-only), and a change rebuilds its users.
BUILD_NUMBER := $(shell date -u +%Y%m%d%H%M)
$(BUILD)/dcr_build.h: FORCE | $(BUILD)
	@echo '#define DCR_BUILD $(BUILD_NUMBER)ULL' > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@
RT_BUILD_H_USERS := dcr_setup dcr_config rt_cfg rt_boot
BUILD_H_USERS := $(sort $(RT_BUILD_H_USERS) $(basename $(notdir $(PORT_BUILD_H_USERS))))
$(foreach u,$(BUILD_H_USERS),$(BUILD)/$(u).o $(BUILD)/rt/$(u).o): $(BUILD)/dcr_build.h

$(BUILD)/%.o: $(SOURCES)/%.c $(RENDERER_STAMP) | $(BUILD) $(BUILD)/dcr_build.h
	@echo $(notdir $<)
	@$(CC) -MMD -MP $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(SOURCES)/%.S $(RENDERER_STAMP) | $(BUILD) $(BUILD)/dcr_build.h
	@echo $(notdir $<)
	@$(CC) -MMD -MP $(ASFLAGS) -c $< -o $@

$(BUILD)/rt/%.o: $(RT_SRC)/%.c $(RENDERER_STAMP) | $(BUILD)/rt $(BUILD)/dcr_build.h
	@echo rt/$(notdir $<)
	@$(CC) -MMD -MP $(CFLAGS) -c $< -o $@

$(BUILD)/rt/%.o: $(RT_SRC)/%.S $(RENDERER_STAMP) | $(BUILD)/rt $(BUILD)/dcr_build.h
	@echo rt/$(notdir $<)
	@$(CC) -MMD -MP $(ASFLAGS) -c $< -o $@

# The relocator runs before BSS/TLS exist and before its own relocations are
# applied: no builtins may turn its loops into memset/memcpy calls (the
# runtime's copy, or a port's own). CRT0_EXTRA=-DDCR_TEST_ALIAS_RELOC (delete
# the crt0_reloc.o first) makes a test build that takes the hardware alias
# path under an emulator too.
$(BUILD)/crt0_reloc.o $(BUILD)/rt/crt0_reloc.o: CFLAGS += -fno-builtin -fno-tree-loop-distribute-patterns $(CRT0_EXTRA)

$(TARGET).elf: $(OFILES)
	@echo linking $@
	@$(CC) $(LDFLAGS) $(OFILES) $(LIBS) -o $@

$(BUILD)/$(TARGET).nso: $(TARGET).elf | $(BUILD)
	@elf2nso $< $@ >/dev/null

# The NPDM: runtime/npdm.json.in with the port's values (the same for every
# port but these), rewritten only when they change.
PORT_NPDM_VERSION    ?= 0.0.3
PORT_NPDM_MAIN_STACK ?= 0x800000
PORT_NPDM_ADDRSPACE  ?= 0
PORT_NPDM_NAME       ?= $(TARGET)
ifneq ($(strip $(PORT_NPDM_JSON)),)
NPDM_JSON := $(PORT_NPDM_JSON)
else
NPDM_JSON := $(BUILD)/$(TARGET).json
ifeq ($(filter clean rt-files,$(MAKECMDGOALS)),)
ifeq ($(strip $(PORT_NPDM_PROGRAM_ID)),)
$(error "The port's Makefile sets PORT_NPDM_PROGRAM_ID (0x01000000000010xx, one per port)")
endif
endif
$(NPDM_JSON): $(RT_DIR)/npdm.json.in FORCE | $(BUILD)
	@sed -e 's/@NAME@/$(PORT_NPDM_NAME)/g' -e 's/@VERSION@/$(PORT_NPDM_VERSION)/g' \
	     -e 's/@PROGRAM_ID@/$(PORT_NPDM_PROGRAM_ID)/g' -e 's/@MAIN_STACK@/$(PORT_NPDM_MAIN_STACK)/g' \
	     -e 's/@ADDRSPACE@/$(PORT_NPDM_ADDRSPACE)/g' $< > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@
endif

$(BUILD)/$(TARGET).npdm: $(NPDM_JSON) | $(BUILD)
	@npdmtool $< $@ 2>&1 | grep -v "Failed to get" || true

$(TARGET).nsp: $(BUILD)/$(TARGET).nso $(BUILD)/$(TARGET).npdm
	@rm -rf $(BUILD)/exefs && mkdir -p $(BUILD)/exefs
	@cp $(BUILD)/$(TARGET).nso $(BUILD)/exefs/main
	@cp $(BUILD)/$(TARGET).npdm $(BUILD)/exefs/main.npdm
	@build_pfs0 $(BUILD)/exefs $@ >/dev/null
	@echo built ... $@

$(TARGET).build: $(TARGET).nsp
	@sed -n 's/.*DCR_BUILD \([0-9]*\)ULL.*/\1/p' $(BUILD)/dcr_build.h > $@
	@echo build number `cat $@`

clean:
	@rm -rf $(BUILD) $(TARGET).elf $(TARGET).nsp $(TARGET).build $(PORT_CLEAN)

-include $(DEPS)
