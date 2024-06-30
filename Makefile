# SPDX-License-Identifier:  MIT
# Copyright 2023-2024 Jorengarenar
# Makefile dialect: GNU

# ~ ----------------------------------------------------------------------- {{{1

.PHONY: regular dev debug build clean stderr scan-build compile_commands.json

cache_build = @ echo "$@:" > $(BUILD)/.target

# VARS -------------------------------------------------------------------- {{{1

EXE := $(notdir $(CURDIR))

SRCDIR   := src
BUILD    := build
OBJDIR   := $(BUILD)/obj
BINDIR   := $(BUILD)/bin
DEPSDIR  := $(BUILD)/deps
DUMPDIR  := $(BUILD)/dump
GENDIR   := $(BUILD)/src

LIBS := pixman-1 wayland-client wayland-cursor

WL_SCANNER = wayland-scanner

CFLAGS   += -std=c2x
CPPFLAGS += -D_POSIX_C_SOURCE -D_GNU_SOURCE
CPPFLAGS += -I$(SRCDIR)
CPPFLAGS += -I$(GENDIR)
CPPFLAGS += -I3rd-party/nanosvg/src

LDFLAGS  +=
LDLIBS   += -lm

SANS += address bounds leak signed-integer-overflow undefined unreachable

SRCS := $(wildcard $(SRCDIR)/*.c)
OBJS := $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(SRCS))

WL_PROT_DATADIR := $(shell pkg-config --variable=pkgdatadir wayland-protocols)

XMLS = \
	external/wlr-layer-shell-unstable-v1.xml \
	$(WL_PROT_DATADIR)/stable/xdg-shell/xdg-shell.xml

PROTS = $(addprefix $(GENDIR)/, \
		   $(foreach file,$(XMLS), \
		     $(notdir $(file:.xml=.c)) \
		     $(notdir $(file:.xml=.h)) \
		   ) \
		 )
PROTS_H += $(filter %.h,$(PROTS))
PROTS_C = $(filter %.c,$(PROTS))

SRCS += $(PROTS_C)
OBJS += $(patsubst $(GENDIR)/%.c, $(OBJDIR)/%.o, $(PROTS_C))


ifneq ($(LIBS),)
	CFLAGS   += $(shell pkg-config --cflags-only-other $(LIBS))
	CPPFLAGS += $(shell pkg-config --cflags-only-I $(LIBS))
	LDFLAGS  += $(shell pkg-config --libs-only-L $(LIBS))
	LDLIBS   += $(shell pkg-config --libs-only-l $(LIBS))
endif

# BUILDS ------------------------------------------------------------------ {{{1

-include $(BUILD)/.target


regular: CFLAGS += -O2 -flto -DNDEBUG
regular: LDFLAGS += -s -flto
regular: build
	$(cache_build)


native: CFLAGS += -march=native -mtune=native
native: regular
	$(cache_build)


dev: CFLAGS += \
	-O3 -flto \
	-march=native -mtune=native
dev: CFLAGS += \
	-Wall -Wextra \
	# -fanalyzer
dev: CFLAGS += \
	-pedantic # -pedantic-errors
dev: CFLAGS += \
	-Wcast-qual \
	-Wcast-align \
	-Wdouble-promotion \
	-Wuseless-cast
dev: CFLAGS += \
	-Wlogical-op \
	-Wfloat-equal
dev: CFLAGS += \
	-Wformat=2 \
	-Wwrite-strings
dev: CFLAGS += \
	-Winline \
	-Wmissing-prototypes \
	-Wstrict-prototypes \
	-Wold-style-definition \
	-Werror=implicit-function-declaration \
	-Werror=return-type
dev: CFLAGS += \
	-Wshadow \
	-Wnested-externs \
	-Werror=init-self
dev: CFLAGS += \
	-Wnull-dereference \
	-Wchar-subscripts \
	-Wsequence-point \
	-Wpointer-arith
dev: CFLAGS += \
	-Wduplicated-cond \
	-Wduplicated-branches
dev: CFLAGS += \
	-Walloca \
	-Werror=vla-larger-than=0
dev: CFLAGS += \
	-Werror=parentheses \
	-Werror=missing-braces \
	-Werror=misleading-indentation
dev: CFLAGS += \
	-g \
	-fno-omit-frame-pointer \
	-fsanitize=$(subst $(eval) ,$(shell echo ","),$(SANS))
dev: build
	$(cache_build)


debug: CFLAGS += \
	-Og \
	-g3 -ggdb3
debug: CFLAGS += \
	-masm=intel -fverbose-asm \
	-save-temps -dumpbase $(DUMPDIR)/$(*F)
debug: build
	$(cache_build)

build: $(BINDIR)/$(EXE)


# RULES ------------------------------------------------------------------- {{{1

$(SRCS): $(GENDIR)/version.h $(PROTS_H)

$(BINDIR)/%: $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(GENDIR)/version.h:
	@mkdir -p $(GENDIR)
	printf '#define WBG_VERSION "%s"' \
		$$(git describe --always --dirty --long) \
		> $@

$(GENDIR)/%.h: $(XMLS)
	@mkdir -p $(GENDIR)
	$(WL_SCANNER) client-header $(filter %/$(notdir $(@:.h=.xml)),$(XMLS)) $@

$(GENDIR)/%.c: $(XMLS)
	@mkdir -p $(GENDIR)
	$(WL_SCANNER) private-code $(filter %/$(notdir $(@:.c=.xml)),$(XMLS)) $@

VPATH = $(GENDIR) $(SRCDIR)
$(OBJDIR)/%.o: %.c
	@mkdir -p $(OBJDIR)
	@mkdir -p $(DUMPDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ -c $<

$(DEPSDIR)/%.d: $(SRCDIR)/%.c
	@mkdir -p $(DEPSDIR)
	$(CC) $(CPPFLAGS) -M $< -MT $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $<) > $@

-include $(patsubst $(OBJDIR)/%.o, $(DEPSDIR)/%.d, $(OBJS))

# MISC -------------------------------------------------------------------- {{{1

clean:
	@ [ "$(CURDIR)" != "$(abspath $(BUILD))" ]
	$(RM) -r $(BUILD)

stderr:
	$(MAKE) $(filter-out $@,$(MAKECMDGOALS)) 2> $(BUILD)/stderr.log
	@ false

scan-build:
	@mkdir -p $(BUILD)/scan-build
	scan-build -o $(BUILD)/scan-build $(MAKE) $(filter-out $@,$(MAKECMDGOALS))

compile_commands.json:
	@ $(MAKE) --always-make --dry-run dev \
		| grep -wE -e '$(CC)' \
		| grep -w -e '\-c' -e '\-x' \
		| jq -nR '[inputs|{command:., directory:"'$$PWD'", file: match("(?<=-c )\\S+").string}]' \
		> compile_commands.json