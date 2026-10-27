#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

GAME_TITLE     := Legend of Zelda
GAME_SUBTITLE1 := A Link to the Past
GAME_SUBTITLE2 := Nintendo

ifeq ($(origin LTTP_ROOT),undefined)
  LTTP_ROOT := $(CURDIR)
endif
export LTTP_ROOT

include $(DEVKITARM)/ds_rules

ifeq ($(strip $(PYTHON)),)
  PYTHON := $(shell command -v python3 2>/dev/null || command -v python 2>/dev/null)
endif
export PYTHON

ARM7_ELF := $(LTTP_ROOT)/platform/nds/arm7/zelda_arm7.elf
_ARM7_ELF := -7 $(ARM7_ELF)

#---------------------------------------------------------------------------------
TARGET   := lttp-ds
BUILD    := build
SOURCES  := platform/nds/source
INCLUDES := platform/nds/include vendor/zelda3 vendor/zelda3/src vendor/zelda3/snes
DATA     :=
GRAPHICS := platform/nds/bottom-screen/images
AUDIO    :=
# 32x32 PNG. The build quantizes it to the 16-color banner BMP.
ICON     := icon.png
ZELDA3   := vendor/zelda3

ZELDA_SRC := $(wildcard $(ZELDA3)/src/*.c) $(wildcard $(ZELDA3)/snes/*.c)
ZELDA_SRC := $(filter-out \
	$(ZELDA3)/src/main.c \
	$(ZELDA3)/src/opengl.c \
	$(ZELDA3)/src/glsl_shader.c \
	$(ZELDA3)/src/zelda_cpu_infra.c \
	$(ZELDA3)/snes/tracing.c,\
	$(ZELDA_SRC))

#---------------------------------------------------------------------------------
ARCH := -march=armv5te -mtune=arm946e-s -marm

CFLAGS   := -g -Wall -O2 -ffunction-sections -fdata-sections\
            -fsigned-char -fno-strict-aliasing -mno-unaligned-access\
            -Wno-deprecated-declarations\
            -include $(LTTP_ROOT)/platform/nds/include/nds_force_types.h\
            $(ARCH) $(INCLUDE) -DARM9 -DLTTP_PPU_RGB15\
            -DCAMERA_PAD_Y=16
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=ds_arm9.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) \
            -Wl,--wrap=FindIndexInMemblk,--wrap=ppu_runLine,--wrap=ZeldaReadSram,--wrap=ZeldaWriteSram

LIBS := -lfat -lnds9 -lm

LIBDIRS := $(LIBNDS) $(PORTLIBS)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT := $(CURDIR)/$(TARGET)

export VPATH := $(CURDIR)/$(subst /,,$(dir $(ICON)))\
                $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))\
                $(foreach dir,$(DATA),$(CURDIR)/$(dir))\
                $(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir))\
                $(CURDIR)/$(ZELDA3)/src \
                $(CURDIR)/$(ZELDA3)/snes

export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c))) \
            $(notdir $(ZELDA_SRC))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PNGFILES := $(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.png)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
  export LD := $(CC)
else
  export LD := $(CXX)
endif

export OFILES_BIN   :=	$(addsuffix .o,$(BINFILES))
# ppu.itcm.o matches the DS linker's *.itcm.* rule, so the software
# renderer executes from instruction RAM instead of main memory.
export OFILES_SOURCES := $(filter-out ppu.o,$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)) ppu.itcm.o
export OFILES := $(PNGFILES:.png=.o) $(OFILES_BIN) zelda3_assets.dat.o $(OFILES_SOURCES)
export HFILES := $(PNGFILES:.png=.h) $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir))\
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include)\
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
  icons := $(wildcard *.bmp)
  ifneq (,$(findstring $(TARGET).bmp,$(icons)))
    export GAME_ICON := $(CURDIR)/$(TARGET).bmp
  else
    ifneq (,$(findstring icon.bmp,$(icons)))
      export GAME_ICON := $(CURDIR)/icon.bmp
    endif
  endif
else ifeq ($(suffix $(ICON)),.grf)
  export GAME_ICON := $(CURDIR)/$(ICON)
else ifeq ($(suffix $(ICON)),.bmp)
  export GAME_ICON := $(CURDIR)/$(ICON)
else ifeq ($(suffix $(ICON)),.png)
  export GAME_ICON := $(CURDIR)/$(BUILD)/$(notdir $(basename $(ICON))).bmp
else
  export GAME_ICON := $(CURDIR)/$(BUILD)/$(notdir $(basename $(ICON))).grf
endif

.PHONY: $(BUILD) clean prepare-assets

$(BUILD): prepare-assets
	@mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# zelda3_assets.dat from the project root, data/, or vendor/zelda3/, or
# extracted from a USA ROM. Linked into the ARM9 binary below.
prepare-assets:
	$(SILENTMSG) assets
	$(SILENTCMD)"$(PYTHON)" "$(LTTP_ROOT)/platform/nds/prepare_assets.py"

clean:
	@echo clean ...
	@$(RM) -r $(BUILD) $(TARGET).elf $(TARGET).nds
	@$(MAKE) -C platform/nds/arm7 clean

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

$(OUTPUT).nds: $(OUTPUT).elf $(GAME_ICON) $(ARM7_ELF)
$(OUTPUT).elf: $(OFILES)

ifeq ($(suffix $(ICON)),.png)
$(GAME_ICON): $(LTTP_ROOT)/$(ICON) $(LTTP_ROOT)/platform/nds/png2banner.py
	$(SILENTMSG) banner $(notdir $@)
	$(SILENTCMD)"$(PYTHON)" "$(LTTP_ROOT)/platform/nds/png2banner.py" $< $@
endif

# No file, so the ARM7 makefile runs on every build and replaces a voice
# driver that was left behind without a dependency file.
.PHONY: force-arm7
force-arm7:

$(ARM7_ELF): force-arm7
	$(MAKE) -C $(LTTP_ROOT)/platform/nds/arm7

$(OFILES_SOURCES) : $(HFILES)

ppu.itcm.o: ppu.c
	$(SILENTMSG) $(notdir $<)
	$(ADD_COMPILE_COMMAND) add $(CC) "$(_EXTRADEFS) $(CPPFLAGS) $(CFLAGS) -c $< -o $@" $<
	$(SILENTCMD)$(CC) -MMD -MP -MF $(DEPSDIR)/ppu.itcm.d $(_EXTRADEFS) $(CPPFLAGS) $(CFLAGS) -c $< -o $@ $(ERROR_FILTER)

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

# .incbin keeps the 684 KB asset file out of a byte-by-byte listing.
# The path is relative to build/, where this makefile runs.
zelda3_assets.dat.o: embed/zelda3_assets.dat
	@echo zelda3_assets.dat
	$(SILENTCMD)printf '%s\n' \
		'.section .rodata' \
		'.balign 4' \
		'.global zelda3_assets_dat' \
		'.global zelda3_assets_dat_end' \
		'zelda3_assets_dat:' \
		'.incbin "embed/zelda3_assets.dat"' \
		'zelda3_assets_dat_end:' \
		> zelda3_assets.s
	$(SILENTCMD)$(CC) $(ASFLAGS) -c zelda3_assets.s -o $@

# grit ignores alpha, so transparent pixels are matted to FF00FF first.
%.s %.h: %.png %.grit
	$(SILENTCMD)"$(PYTHON)" "$(LTTP_ROOT)/platform/nds/png_matte.py" $< $*.matte.png
	$(DEVKITPRO)/tools/bin/grit $*.matte.png -ff $(filter %.grit,$^) -fts -o$*
	@$(RM) $*.matte.png

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
