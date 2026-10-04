CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Wpedantic -std=c11 -O3 -fno-math-errno -fno-trapping-math
AR      ?= ar

SRC_DIR   = src
INC_DIR   = include
TEST_DIR  = tests
BUILD_DIR = build

LIB_SRCS = $(SRC_DIR)/vida_lexer.c $(SRC_DIR)/vida_value.c $(SRC_DIR)/vida_vm.c $(SRC_DIR)/vida_compiler.c $(SRC_DIR)/vida_native.c
LIB_OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(LIB_SRCS))
HEADERS = $(INC_DIR)/vida_lexer.h $(INC_DIR)/vida_value.h $(INC_DIR)/vida_vm.h $(INC_DIR)/vida_compiler.h $(INC_DIR)/vida_native.h
LDLIBS = -lm

STATIC_LIB = $(BUILD_DIR)/libvida.a
VDA_BIN    = $(BUILD_DIR)/vda

.PHONY: all clean test

all: $(VDA_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(INC_DIR) -c $< -o $@

$(STATIC_LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(VDA_BIN): $(BUILD_DIR)/main.o $(STATIC_LIB)
	$(CC) $(CFLAGS) -o $@ $< -L$(BUILD_DIR) -lvida $(LDLIBS)

test: $(VDA_BIN)
	@echo "=== vida test suite ==="
	@echo "--- Arithmetic ---"
	@./$(VDA_BIN) eval "print(2 + 3)"
	@./$(VDA_BIN) eval "print(10 - 4)"
	@./$(VDA_BIN) eval "print(6 * 7)"
	@./$(VDA_BIN) eval "print(15 / 3)"
	@echo "--- Variables ---"
	@./$(VDA_BIN) eval 'let x = 42; print(x)'
	@echo "--- Strings ---"
	@./$(VDA_BIN) eval 'print("hello " + "vida!")'
	@echo "--- Booleans ---"
	@./$(VDA_BIN) eval 'print(true and false)'
	@./$(VDA_BIN) eval 'print(true or false)'
	@echo "--- Functions ---"
	@./$(VDA_BIN) eval 'fn add(a, b) { return a + b } print(add(3, 4))'
	@./$(VDA_BIN) eval 'fn fact(n) { if n <= 1 { return 1 } return n * fact(n - 1) } print(fact(10))'
	@./$(VDA_BIN) eval 'print(sqrt(3 * 3 + 4 * 4))'
	@echo "--- All tests done! ---"

clean:
	rm -rf $(BUILD_DIR)
