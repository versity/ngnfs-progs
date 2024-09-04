# SPDX-License-Identifier: GPL-2.0

CFLAGS := -I. -O2 -ggdb -Wall -Werror -D_FILE_OFFSET_BITS=64 -msse4.2 -fno-strict-aliasing -fno-omit-frame-pointer

# function entrance/exit profiling for uftrace
CFLAGS += -pg

# produce .i and .s
CFLAGS += -save-temps

LDFLAGS := -Wl,--gc-sections -lurcu-common -lurcu -lurcu-cds

# provide dynamic symbol tables for backtrace()
LDFLAGS += -rdynamic

# build all c files in source directories
DIR := manifest cli devd shared shared/lk
SRC := $(foreach d,$(DIR),$(wildcard $(d)/*.c))
OBJ := $(patsubst %.c,%.o,$(SRC))
DEP := $(foreach d,$(DIR),$(wildcard $(d)/*.d))

# source with main() is linked as a binary
BIN := $(patsubst %.c,%,$(shell grep -l "^int main" $(SRC)))

# binary names have ngnfs- prefixed
#binname = $(dir $1)ngnfs-$(notdir $1)

# check exposed format structs by building compiled headers with -Wpadded
GCH := $(patsubst %,%.gch,$(wildcard shared/format-*.h))

.PHONY: all
all: shared/generated-trace-inlines.h $(GCH) $(BIN)

ifneq ($(DEP),)
-include $(DEP)
endif

# link binaries with all shared objs, removing unused symbols
PERCENT := %
.SECONDEXPANSION:
$(BIN): %: %.o 	$$(filter $$(dir %)$$(PERCENT),$$(OBJ)) \
		$$(filter shared/$$(PERCENT),$$(OBJ)) \
		$$(filter shared/lk/$$(PERCENT),$$(OBJ))
	gcc $(LDFLAGS) -o $@ $^

#	gcc $(LDFLAGS) -o $(call binname,$@) $^

%.o %.d: %.c Makefile
	gcc $(CFLAGS) -MD -MP -MF $*.d -c $< -o $*.o
	./scripts/sparse.sh -Wbitwise -D__CHECKER__ $(CFLAGS) $<

$(GCH): %.h.gch: %.h Makefile
	gcc $(CFLAGS) -Wpadded -c $< -o $@

shared/generated-trace-inlines.h: scripts/generate-trace-events.awk \
				  shared/trace-events.txt Makefile
	gawk -f $< < shared/trace-events.txt > $@

.PHONY: clean
clean:
	@rm -f $(BIN) $(OBJ) $(DEP) $(GCH) \
		$(foreach d,$(DIR),$(wildcard $(d)/*.[is])) \
		shared/generated-trace-inlines.h \
		.sparse.gcc-defines.h .sparse.output

