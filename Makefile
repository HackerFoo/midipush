-include config.mk

SHELL := bash
ROOT := $(dir $(realpath $(firstword $(MAKEFILE_LIST))))

# defaults
BUILD ?= release-with-asserts

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
	UNAME_O := $(shell uname -o)
endif

ifeq ($(CC),cc)
	CC=clang
endif

ifeq ($(findstring gcc, $(CC)),gcc)
	SANITIZE := -fsanitize=undefined
	CFLAGS = -falign-functions=4 -Wall -std=gnu99
	OPT_FLAG=-O3
	LDFLAGS += -rdynamic
endif
ifeq ($(findstring clang, $(CC)),clang)
	SANITIZE := -fsanitize=undefined -fno-sanitize=bounds
	CFLAGS = -Wall -Wextra -pedantic -std=gnu11 \
                 -Wno-gnu-zero-variadic-macro-arguments -Wno-address-of-packed-member \
                 -Wno-unknown-warning-option -Wno-zero-length-array -Wno-array-bounds \
                 -Werror=implicit-function-declaration -Werror=int-conversion \
                 -Wno-unused-parameter -Wno-variadic-macros
	CXXFLAGS = -xc++ -Wall -Wextra -pedantic -std=c++98
	OPT_FLAG=-O3
	LDFLAGS += -rdynamic
endif

ifeq ($(BUILD),debug)
	OPT_FLAG = -O0
	CFLAGS += -g $(OPT_FLAG) $(SANITIZE)
	CXXFLAGS += -g $(OPT_FLAG) $(SANITIZE)
	LIBS += $(SANITIZE)
endif

ifeq ($(BUILD),release)
	CFLAGS += -DNDEBUG $(OPT_FLAG)
	CXXFLAGS += -DNDEBUG $(OPT_FLAG)
endif

ifeq ($(BUILD),release-with-asserts)
	CFLAGS += $(OPT_FLAG)
	CXXFLAGS += $(OPT_FLAG)
endif

ifeq ($(BUILD),profile)
	CFLAGS += -DNDEBUG $(OPT_FLAG)
	CXXFLAGS += -DNDEBUG $(OPT_FLAG)
	LIBS += -lprofiler
endif

ifeq ($(BUILD),gprof)
	CFLAGS += -DNDEBUG -pg $(OPT_FLAG)
	CXXFLAGS += -DNDEBUG -pg $(OPT_FLAG)
	LDFLAGS += -pg
endif

INCLUDE += -I.gen
CFLAGS += $(COPT) $(INCLUDE)
CXXFLAGS += $(COPT) $(INCLUDE)

BUILD_DIR := build/$(CC)/$(BUILD)

SRC := $(wildcard *.c) $(wildcard startle/*.c) dtask/src/dtask.c
OBJS := $(patsubst %.c, $(BUILD_DIR)/%.o, $(SRC))
DEPS := $(patsubst %.c, $(BUILD_DIR)/%.d, $(SRC))

# DTask stuff
DTASK_ROOT := dtask
DTASK_SRC := $(DTASK_ROOT)/src
DTASK_TOOLS := $(DTASK_ROOT)/tools

TASK_SRC := $(wildcard *.c)
DTASK_TARGETS := midi_tasks
DTASK_GENERATED_HEADERS := $(patsubst %, .gen/%.h, $(DTASK_TARGETS))
INCLUDE += -I $(DTASK_SRC)

LIBS += -lasound -lz

.PHONY: all
all: midipush

print-%:
	@echo $($*)

midipush: $(DTASK_GENERATED_HEADERS) $(BUILD_DIR)/midipush
	ln -fs $(BUILD_DIR)/midipush $@

$(DTASK_GENERATED_HEADERS): .gen/%.h : $(TASK_SRC)
	@mkdir -p .gen
	PYTHONPATH=$(DTASK_ROOT) python $(DTASK_TOOLS)/generate_task_header.py -I $(DTASK_SRC) --target $* $(TASK_SRC) -o .gen/$*.h.tmp
	mv .gen/$*.h.tmp .gen/$*.h

include startle/startle.mk

# link
$(BUILD_DIR)/midipush: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) $(LIBS) -o $@

# remove compilation products
.PHONY: clean
clean:
	rm -rf build .gen midipush
