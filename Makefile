# Makefile — MemoryGraph SDK (macOS host build)
#
# Usage:
#   make cli   — build mg2crash command-line tool
#   make clean — remove build artifacts

CC       := clang
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -g -O0 -MMD -MP

BUILD_DIR := build

# ---------- Rules ----------

.PHONY: cli clean

cli: $(BUILD_DIR)/mg2crash

$(BUILD_DIR)/mg2crash: $(BUILD_DIR)/src/mg_binary_reader.o $(BUILD_DIR)/tools/mg2crash.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

-include $(wildcard $(BUILD_DIR)/**/*.d)
