# ==============================================================================
#
# TL2 STM Backend Makefile - Modernized for C++17
#
# ==============================================================================

# ==============================================================================
# Variables
# ==============================================================================

CC      := clang
CPP     := clang++
CFLAGS  := -g -O0 -std=c99 -pthread -fPIC
CXXFLAGS := -g -O0 -std=c++17 -pthread -fPIC
CFLAGS  += -DTL2_STATS
CFLAGS  += -DNDEBUG
CFLAGS  += -I.

LD      := clang

# Directory structure
OUT_DIR := out
LIB_DIR := lib

# Final library
LIBTL2 := $(LIB_DIR)/libtl2.a

SRCS := tl2.c tmalloc.c
OBJS := $(patsubst %.c,$(OUT_DIR)/%.o,$(SRCS))

AR      := ar
RANLIB  := ranlib

RM := rm -rf
MKDIR := mkdir -p

# ==============================================================================
# Rules
# ==============================================================================

.PHONY: default all
default: all
all: $(LIB_DIR) $(OUT_DIR) $(LIBTL2)

.PHONY: clean
clean:
	$(RM) $(OUT_DIR) $(LIB_DIR)

$(OUT_DIR):
	$(MKDIR) $(OUT_DIR)

$(LIB_DIR):
	$(MKDIR) $(LIB_DIR)

$(OUT_DIR)/%.o: %.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBTL2): $(OBJS) | $(LIB_DIR)
	$(AR) cru $@ $^
	$(RANLIB) $@

.PHONY: lazy
lazy: $(LIBTL2)

.PHONY: eager
eager: CFLAGS += -DTL2_EAGER
eager: $(LIBTL2)

.PHONY: lazy-nocm
lazy-nocm: CFLAGS += -DTL2_NOCM
lazy-nocm: $(LIBTL2)

.PHONY: eager-nocm
eager-nocm: CFLAGS += -DTL2_EAGER -DTL2_NOCM
eager-nocm: $(LIBTL2)

# ==============================================================================
# Dependencies
# ==============================================================================

$(OUT_DIR)/tl2.o: tl2.h platform.h common.h
$(OUT_DIR)/tmalloc.o: tmalloc.h

# ==============================================================================
#
# End of Makefile
#
# ==============================================================================
