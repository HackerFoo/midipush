LDFLAGS := -lasound

BUILD_DIR := build
DTASK_ROOT := dtask
DTASK_SRC := $(DTASK_ROOT)/src
DTASK_TOOLS := $(DTASK_ROOT)/tools

SRC := $(wildcard *.c)
DTASK_TARGETS := midi_tasks
DTASK_GENERATED_HEADERS := $(patsubst %, $(BUILD_DIR)/%.h, $(DTASK_TARGETS))
HEADERS := $(wildcard *.h) $(DTASK_GENERATED_HEADERS)
OBJS := $(patsubst %.c, $(BUILD_DIR)/%.o, $(SRC)) $(BUILD_DIR)/dtask.o
CFLAGS := -g -O3 -std=c99 -I $(DTASK_SRC) -I $(BUILD_DIR)

.PHONY: all
all: midipush

print-%:
	@echo $($*)

midipush: $(OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

$(DTASK_GENERATED_HEADERS): $(BUILD_DIR)/%.h : $(SRC)
	@mkdir -p $(BUILD_DIR)
	PYTHONPATH=$(DTASK_ROOT) python $(DTASK_TOOLS)/generate_task_header.py -I $(DTASK_SRC) --target $* $(SRC)
	mv $*.h $(BUILD_DIR)

$(BUILD_DIR)/%.o: %.c $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $< -o $@ $(CFLAGS) $(CFLAGS_EXT)

$(BUILD_DIR)/dtask.o: $(DTASK_SRC)/dtask.c $(DTASK_SRC)/*.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $< -o $@ $(CFLAGS) $(CFLAGS_EXT)

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
	rm -f test
