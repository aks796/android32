#---------------------------------------------------------------------------------
# launcher.mk -- the shared launcher NRO (64-bit, devkitA64 + libnx), included
# by a port's launcher/Makefile:
#
#   APP_TITLE    := Labyrinth 2            # the NRO's name (sphaira's list)
#   APP_AUTHOR   := aks796, Illusion Labs
#   APP_VERSION  := 1.0.0
#   TARGET       := labyrinth2_nx          # the NRO: PORT_NAME
#   PORT_PAYLOAD := labyrinth2_nx          # the 32-bit program: the wrapper's TARGET
#   # optional:
#   #   ICON             (icon.jpg)
#   #   LAUNCHER_SOURCES (source): folders of the port's hooks (launcher.h), *.c
#   #   LAUNCHER_LIBS    more libraries (a8r: -lminizip -lz)
#   TOPDIR ?= $(CURDIR)
#   A32    ?= $(TOPDIR)/../runtime
#   include $(A32)/launcher/launcher.mk
#
# The NRO carries ../$(PORT_PAYLOAD).nsp and ../$(PORT_PAYLOAD).build (built
# first by the port's ../build.sh) in its romfs, with whatever else the port
# puts in romfs/ (launcher/romfs_extras.sh, run by build.sh). The payload's
# name is a contract with the game program's self-update: never rename it.
#
# Sources: the port's LAUNCHER_SOURCES, then the shared launcher
# ($(A32)/launcher/source; a port file of the same name replaces a shared
# one), and the runtime's rt_migrate.c and rt_apkfind.c. Headers: the same
# folders, the port's ../source (port_config.h, and any runtime header the
# port overrides) and the runtime's source/ (rt_settings.h, dcr_exefs.h...).
# The rest is the libnx application template.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif
ifeq ($(strip $(PORT_PAYLOAD)),)
$(error "launcher.mk: set PORT_PAYLOAD (the 32-bit program's name) in the port's launcher/Makefile")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

ICON             ?= icon.jpg
LAUNCHER_SOURCES ?= source
BUILD            := build
DATA             := data
ROMFS            := romfs

A32_LAUNCHER := $(A32)/launcher/source
A32_SOURCE   := $(A32)/source
PORT_DIRS    := $(foreach dir,$(LAUNCHER_SOURCES),$(TOPDIR)/$(dir))

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# The title goes into a C string through the shell (and through devkitPro's
# rules, which echo CFLAGS inside "..."): characters the shell would act on
# become octal escapes ("Sonic & SEGA All-Stars Racing").
LP_OPEN  := (
LP_CLOSE := )
LAUNCHER_TITLE_C := $(subst ",\042,$(subst ',\047,$(subst &,\046,$(subst ;,\073,$(subst |,\174,$(subst $(LP_OPEN),\050,$(subst $(LP_CLOSE),\051,$(subst <,\074,$(subst >,\076,$(APP_TITLE))))))))))
DEFINES	:=	-DPORT_PAYLOAD_NAME=\"$(PORT_PAYLOAD)\" '-DLAUNCHER_APP_TITLE="$(LAUNCHER_TITLE_C)"'

CFLAGS	:=	-g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -O2 \
			-ffunction-sections $(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= $(LAUNCHER_LIBS) -lnx

LIBDIRS	:= $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

# the port's folders first: its file of a shared file's name wins
export VPATH	:=	$(PORT_DIRS) $(A32_LAUNCHER) $(A32_SOURCE) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

PORT_CFILES	:=	$(foreach dir,$(PORT_DIRS),$(notdir $(wildcard $(dir)/*.c)))
A32_CFILES	:=	$(filter-out $(PORT_CFILES),$(notdir $(wildcard $(A32_LAUNCHER)/*.c)) rt_migrate.c rt_apkfind.c)
CFILES		:=	$(PORT_CFILES) $(A32_CFILES)
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export LD	:=	$(CC)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CFILES:.c=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(PORT_DIRS),-I$(dir)) -I$(A32_LAUNCHER) \
			-I$(TOPDIR)/../source -I$(A32_SOURCE) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_ICON := $(TOPDIR)/$(ICON)
export NROFLAGS += --icon=$(APP_ICON)
export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif
export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

# the wrapper and its build number, from the wrapper build next door
PAYLOAD := $(ROMFS)/$(PORT_PAYLOAD).nsp $(ROMFS)/$(PORT_PAYLOAD).build
$(ROMFS)/%: ../%
	@mkdir -p $(ROMFS)
	@cp $< $@
	@rm -f $(TARGET).nro # the NRO depends only on the ELF: repack it with the new payload

$(BUILD): $(PAYLOAD)
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf $(ROMFS)

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

all	:	$(OUTPUT).nro

$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp

$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
