#---------------------------------------------------------------------------------
# Nintendo Switch build (devkitA64). Run inside devkitpro/devkita64:
#   docker run --rm -v $PWD:/src -w /src devkitpro/devkita64 make
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO not set - build inside the devkitpro/devkita64 image")
endif

include $(DEVKITPRO)/libnx/switch_rules

APP_TITLE   := green-nx
APP_AUTHOR  := rmrf404
APP_VERSION := 1.0.0
APP_ICON    := icon.jpg

TARGET   := green-nx
BUILD    := build-switch
SOURCES  := src/core src/switch
INCLUDES := src vendor
ROMFS    := romfs

DEFINES := -DGNX_VERSION=\"$(APP_VERSION)\"

# Native streaming switches on automatically once the WebRTC deps are built
# (bash deps/build-switch.sh). Until then the app falls back to libnxbox.
GNXROOT := $(dir $(abspath $(firstword $(MAKEFILE_LIST))))
ifneq ($(strip $(wildcard $(GNXROOT)deps/switch/lib/libpeer.a)),)
DEFINES     += -DGNX_NATIVE_STREAM
SOURCES     += src/switch/stream
EXTRA_LIBDIRS := $(GNXROOT)deps/switch
STREAM_LIBS := -lpeer -lsrtp2 -lusrsctp
STREAM_PKGS := libavcodec libavutil opus
# Force a relink whenever the WebRTC archives are rebuilt (deps/build-switch.sh).
# Without this, an incremental `make` keeps the stale .elf because no .cpp
# changed, silently shipping the OLD libpeer.a.
export GNX_STREAM_DEPS := $(wildcard $(GNXROOT)deps/switch/lib/lib*.a)
endif

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS   := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES) $(INCLUDE) \
            -D__SWITCH__
CXXFLAGS := $(CFLAGS) -fno-rtti -std=gnu++17
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
            -Wl,-Map,$(notdir $*.map)

PKGCONF := /opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-pkg-config
LIBS := $(STREAM_LIBS) \
        $(shell $(PKGCONF) --libs $(STREAM_PKGS) SDL2_ttf SDL2_image sdl2 libcurl) \
        -lmbedtls -lmbedx509 -lmbedcrypto \
        -lEGL -lglapi -ldrm_nouveau -ldeko3d -lnx
LIBDIRS := $(EXTRA_LIBDIRS) $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))

export LD := $(CXX)
export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifneq ($(strip $(ROMFS)),)
export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

# switch_rules only *defaults* APP_ICON; the app Makefile must put the icon
# and NACP on the elf2nro command line itself (as the official template does).
export NROFLAGS += --icon=$(CURDIR)/$(APP_ICON) --nacp=$(CURDIR)/$(TARGET).nacp

# deko3d shaders: compiled from GLSL to .dksh with uam and shipped in romfs.
UAM        := $(DEVKITPRO)/tools/bin/uam
SHADER_OUT := $(CURDIR)/romfs/shaders/video_vsh.dksh \
              $(CURDIR)/romfs/shaders/video_fsh.dksh

.PHONY: all clean shaders

all: shaders $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

shaders: $(SHADER_OUT)

$(CURDIR)/romfs/shaders/%_vsh.dksh: $(CURDIR)/shaders/%_vsh.glsl
	@mkdir -p $(CURDIR)/romfs/shaders
	@echo "uam $(notdir $<)"
	@$(UAM) -s vert -o $@ $<

$(CURDIR)/romfs/shaders/%_fsh.dksh: $(CURDIR)/shaders/%_fsh.glsl
	@mkdir -p $(CURDIR)/romfs/shaders
	@echo "uam $(notdir $<)"
	@$(UAM) -s frag -o $@ $<

$(BUILD):
	@mkdir -p $@

clean:
	@rm -rf $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf $(SHADER_OUT)

#---------------------------------------------------------------------------------
else

all: $(OUTPUT).nro

# Relink when our objects OR the prebuilt WebRTC archives change; regenerate
# the NACP (and thus repackage the .nro) when the Makefile/version changes.
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES) $(GNX_STREAM_DEPS)
$(OFILES): $(MAKEFILE_LIST)

%.o: %.cpp
	@echo $(notdir $<)
	$(CXX) -MMD -MP -MF $(DEPSDIR)/$*.d $(CXXFLAGS) -c $< -o $@ $(ERROR_FILTER)

-include $(DEPSDIR)/*.d

endif
