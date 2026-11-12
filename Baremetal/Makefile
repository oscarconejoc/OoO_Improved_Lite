override CC := riscv64-unknown-elf-gcc

MARCH_FLAGS := -march=rv32imf -mabi=ilp32f

CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c99 -Iinclude -ffreestanding -ffp-contract=off $(MARCH_FLAGS)
ASFLAGS ?= -ffreestanding $(MARCH_FLAGS)
LDFLAGS ?= -nostdlib -nostartfiles -ffreestanding $(MARCH_FLAGS) -T link.ld -Wl,--no-warn-rwx-segments

BUILD_DIR := build
SRC_DIR := src
INCLUDE_DIR := include

# start.S va primero para que quede claro que es el punto de entrada
SRCS_C := $(SRC_DIR)/axpy_rvv.c $(SRC_DIR)/gemm_rvv.c $(SRC_DIR)/metrics.c $(SRC_DIR)/result.c $(SRC_DIR)/main.c
SRCS_ASM := $(SRC_DIR)/start.S

OBJS := $(BUILD_DIR)/start.o $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS_C))
TARGET := $(BUILD_DIR)/bare_metal_test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/start.o: $(SRC_DIR)/start.S | $(BUILD_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) gem5/out __pycache__ .pytest_cache .mypy_cache

.PHONY: all clean

