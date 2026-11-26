# Builds the PC reference (hostrun) from an upstream snesrev/smw checkout.
#   make -C tools/hostrun SMW=/path/to/smw
# The game code (smb1/, smbll/) comes from this repository's game/ directory,
# so the reference runs exactly the code the GBA build does; the SNES
# emulator, the PPU and the rest of the PC runtime come from upstream.
SMW ?= ../../third_party/smw
GAME ?= ../../game
CC ?= cc
CFLAGS ?= -O2 -g
SAN ?=
CFLAGS += -fno-strict-aliasing -w -DSYSTEM_VOLUME_MIXER_AVAILABLE=0 -include stddef.h $(SAN)
INC := -I$(SMW) -I$(SMW)/src -I. -Iinclude

UPSTREAM_SRCS := $(filter-out $(SMW)/src/main.c $(SMW)/src/config.c $(SMW)/src/opengl.c $(SMW)/src/glsl_shader.c,$(wildcard $(SMW)/src/*.c)) $(wildcard $(SMW)/src/snes/*.c)
GAME_SRCS := $(wildcard $(GAME)/smb1/*.c $(GAME)/smbll/*.c)

OBJDIR := obj
OBJS := $(patsubst %.c,$(OBJDIR)/up/%.o,$(notdir $(UPSTREAM_SRCS))) \
        $(patsubst %.c,$(OBJDIR)/game/%.o,$(notdir $(GAME_SRCS))) \
        $(OBJDIR)/hostrun.o

vpath %.c $(SMW)/src $(SMW)/src/snes $(GAME)/smb1 $(GAME)/smbll .

all: hostrun

hostrun: $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ -lpng -lz -lm

$(OBJDIR)/up/%.o: %.c | $(OBJDIR)/up
	$(CC) $(CFLAGS) $(INC) -c $< -o $@
$(OBJDIR)/game/%.o: %.c | $(OBJDIR)/game
	$(CC) $(CFLAGS) $(INC) -c $< -o $@
$(OBJDIR)/hostrun.o: hostrun.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

$(OBJDIR) $(OBJDIR)/up $(OBJDIR)/game:
	mkdir -p $@

clean:
	rm -rf $(OBJDIR) hostrun
