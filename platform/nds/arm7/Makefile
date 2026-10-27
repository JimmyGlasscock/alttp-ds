#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

include $(DEVKITARM)/ds_rules

#---------------------------------------------------------------------------------
TARGET   := zelda_arm7
BUILD    := build
SOURCES  := source
INCLUDES := source ../include

#---------------------------------------------------------------------------------
ARCH := -march=armv4t -mtune=arm7tdmi -mthumb

CFLAGS   := -g -Wall -Os -ffunction-sections -fdata-sections\
            $(ARCH) $(INCLUDE) -DARM7
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=ds_arm7.specs -g $(ARCH) -Wl,-Map,$(notdir $*).map

LIBS := -lnds7
LIBDIRS := $(LIBNDS)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export ARM7ELF := $(CURDIR)/$(TARGET).elf
export DEPSDIR := $(CURDIR)/$(BUILD)

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))

ifeq ($(strip $(CPPFILES)),)
  export LD := $(CC)
else
  export LD := $(CXX)
endif

export OFILES := $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-iquote $(CURDIR)/$(dir))\
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include)\
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean arm7 ...
	@rm -fr $(BUILD) $(TARGET).elf

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

DEPENDS := $(OFILES:.o=.d)

# A dependency file written on Windows starts with C:/. GNU make reads
# that colon as a rule and stops with "target pattern contains no '%'".
# The object has to go with it. Leaving the Windows ARM7 objects in
# place compiles a new ARM9 against an old voice driver, and the mixer
# then reads the IPC at the wrong offsets, so no channel ever starts.
ifneq ($(strip $(OFILES)),)
BADDEPS := $(shell grep -l '^[A-Za-z]:' *.d 2>/dev/null)
STALEOBJS := $(BADDEPS:.d=.o)
STALEOBJS += $(foreach o,$(OFILES),$(if $(wildcard $(o:.o=.d)),,$(o)))
STRAY := $(filter-out $(OFILES),$(wildcard *.o) $(wildcard *.16.o))
ifneq ($(strip $(BADDEPS) $(STALEOBJS) $(STRAY)),)
$(shell rm -f $(BADDEPS) $(STALEOBJS) $(STRAY))
endif
endif

$(ARM7ELF): $(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
