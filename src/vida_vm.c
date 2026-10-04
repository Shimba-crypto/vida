#include "vida_vm.h"
#include "vida_native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#if defined(__GNUC__) || defined(__clang__)
#define VIDA_LIKELY(x) __builtin_expect(!!(x), 1)
#define VIDA_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define VIDA_LIKELY(x) (x)
#define VIDA_UNLIKELY(x) (x)
#endif

void vida_chunk_init(VidaChunk *chunk) {
    chunk->code = NULL;
    chunk->lines = NULL;
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->constants = NULL;
    chunk->const_count = 0;
    chunk->const_capacity = 0;
}

void vida_chunk_write(VidaChunk *chunk, uint8_t byte, int line) {
    if (chunk->capacity < chunk->count + 1) {
        int old_capacity = chunk->capacity;
        chunk->capacity = old_capacity < 8 ? 8 : old_capacity * 2;
        chunk->code = realloc(chunk->code, sizeof(uint8_t) * (size_t)chunk->capacity);
        chunk->lines = realloc(chunk->lines, sizeof(int) * (size_t)chunk->capacity);
    }
    chunk->code[chunk->count] = byte;
    chunk->lines[chunk->count] = line;
    chunk->count++;
}

int vida_chunk_add_constant(VidaChunk *chunk, VidaValue value) {
    if (chunk->const_capacity < chunk->const_count + 1) {
        int old_capacity = chunk->const_capacity;
        chunk->const_capacity = old_capacity < 8 ? 8 : old_capacity * 2;
        chunk->constants = realloc(chunk->constants, sizeof(VidaValue) * (size_t)chunk->const_capacity);
    }
    chunk->constants[chunk->const_count] = value;
    return chunk->const_count++;
}

void vida_chunk_free(VidaChunk *chunk) {
    free(chunk->code);
    free(chunk->lines);
    for (int i = 0; i < chunk->const_count; i++) {
        vida_value_free(&chunk->constants[i]);
    }
    free(chunk->constants);
    vida_chunk_init(chunk);
}

VidaFunc *vida_func_new(const char *name, int name_len, int arity) {
    VidaFunc *func = (VidaFunc *)malloc(sizeof(VidaFunc));
    if (!func) return NULL;
    func->name = (char *)malloc((size_t)name_len + 1);
    if (!func->name) { free(func); return NULL; }
    memcpy(func->name, name, (size_t)name_len);
    func->name[name_len] = '\0';
    func->arity = arity;
    vida_chunk_init(&func->chunk);
    func->refcount = 1; // owned by creator (usually a constant pool)
    return func;
}

void vida_func_retain(VidaFunc *func) {
    if (func) func->refcount++;
}

void vida_func_release(VidaFunc *func) {
    if (!func) return;
    if (--func->refcount > 0) return;
    vida_chunk_free(&func->chunk);
    free(func->name);
    free(func);
}

void vida_vm_init(VidaVM *vm) {
    vm->stack_top = 0;
    vm->global_count = 0;
    vm->frame_count = 0;
    vm->chunk = NULL;
    vm->ip = NULL;
    // Register natives first, in canonical table order. The compiler
    // pre-registers the same names in the same order, so slots agree.
    for (int i = 0; i < vida_native_count() && i < VIDA_GLOBALS_MAX; i++) {
        VidaValue v;
        v.type = VAL_NATIVE;
        v.as.native.fn = vida_native_fn(i);
        v.as.native.name = vida_native_name(i);
        vm->globals[vm->global_count++] = v;
    }
}

void vida_vm_free(VidaVM *vm) {
    for (int i = 0; i < vm->stack_top; i++) {
        vida_value_free(&vm->stack[i]);
    }
    for (int i = 0; i < vm->global_count; i++) {
        vida_value_free(&vm->globals[i]);
    }
    vm->stack_top = 0;
    vm->global_count = 0;
}

// --- hot-path inline helpers (no function-call overhead) ---
static inline VidaValue vida_copy_inline(VidaValue v) {
    if (v.type == VAL_STRING) {
        return vida_value_string(v.as.string.chars, v.as.string.length);
    }
    if (v.type == VAL_FUNC) {
        vida_func_retain(v.as.func);
    }
    return v;
}

static inline void vida_free_inline(VidaValue *v) {
    if (v->type == VAL_STRING) {
        free(v->as.string.chars);
        v->as.string.chars = NULL;
        v->as.string.length = 0;
        v->as.string.capacity = 0;
    }
    v->type = VAL_NIL;
}

static inline bool vida_is_falsey_inline(VidaValue v) {
    return v.type == VAL_NIL || (v.type == VAL_BOOL && !v.as.boolean);
}

// Exponential append: turns O(n^2) `s = s + x` loops into amortized O(n).
static inline bool vida_append_inline(VidaValue *dst, VidaValue *src) {
    int new_len = dst->as.string.length + src->as.string.length;
    if (new_len + 1 > dst->as.string.capacity) {
        int new_cap = dst->as.string.capacity ? dst->as.string.capacity : 16;
        while (new_cap <= new_len) new_cap *= 2;
        char *nc = (char *)realloc(dst->as.string.chars, (size_t)new_cap);
        if (!nc) return false;
        dst->as.string.chars = nc;
        dst->as.string.capacity = new_cap;
    }
    memcpy(dst->as.string.chars + dst->as.string.length,
           src->as.string.chars, (size_t)src->as.string.length);
    dst->as.string.length = new_len;
    dst->as.string.chars[new_len] = '\0';
    return true;
}

static void runtime_error_at(VidaVM *vm, VidaChunk *chunk, uint8_t *ip, const char *msg) {
    // ip points past the failing opcode; back up by 1 for line info.
    int instruction = (int)(ip - chunk->code) - 1;
    int line = 0;
    if (instruction >= 0 && instruction < chunk->count) line = chunk->lines[instruction];
    fprintf(stderr, "%s\n[line %d] in script\n", msg, line);
    vm->ip = ip;
}

VidaResult vida_vm_run(VidaVM *vm, VidaChunk *chunk) {
    // Bootstrap: run the script chunk as an arity-0 function frame.
    // Slot 0 is reserved in every frame (callee copy for calls, NIL here).
    if (vm->frame_count >= VIDA_FRAMES_MAX) {
        fprintf(stderr, "Call stack overflow\n");
        return VIDA_RUNTIME_ERROR;
    }
    VidaFunc script; // borrowed wrapper; never freed as a value
    script.name = "script";
    script.arity = 0;
    script.chunk = *chunk; // shallow: shares code/constants/lines
    script.refcount = 1;
    VidaFrame *frame = &vm->frames[vm->frame_count++];
    frame->func = &script;
    frame->ip = chunk->code;
    frame->slots = vm->stack + vm->stack_top;
    vm->stack[vm->stack_top++] = VIDA_NIL(); // reserve slot 0

    vm->chunk = chunk;
    uint8_t *ip = frame->ip;
    VidaValue *stack = vm->stack;
    VidaValue *slots = frame->slots;
    VidaValue *consts = chunk->constants;
    VidaValue *globals = vm->globals;
    int top = vm->stack_top;
    int nglobals = vm->global_count;

#define VIDA_LOAD_FRAME() do { \
    chunk = &frame->func->chunk; \
    consts = chunk->constants; \
    ip = frame->ip; \
    slots = frame->slots; \
    vm->chunk = chunk; \
} while (0)

#define VIDA_SYNC() do { vm->ip = ip; frame->ip = ip; vm->stack_top = top; vm->global_count = nglobals; } while (0)
#define READ_BYTE() (*ip++)
#define READ_SHORT() (ip += 2, (uint16_t)(((uint16_t)ip[-2] << 8) | ip[-1]))
#define READ_I16() ((int16_t)READ_SHORT())
#define PUSH(v) do { \
    if (VIDA_UNLIKELY(top >= VIDA_STACK_MAX)) { \
        VIDA_SYNC(); \
        runtime_error_at(vm, chunk, ip, "Stack overflow"); \
        return VIDA_RUNTIME_ERROR; \
    } \
    stack[top++] = (v); \
} while (0)
#define POP() (stack[--top])
#define PEEK(d) (stack[top - 1 - (d)])

#if defined(__GNUC__) && !defined(VIDA_NO_THREADED_CODE)
    // Direct-threaded dispatch: ~10-15% fewer branch mispredicts than switch.
    static void *dispatch_table[] = {
        [OP_CONSTANT] = &&op_constant,
        [OP_CONSTANT_LONG] = &&op_constant_long,
        [OP_NIL] = &&op_nil,
        [OP_TRUE] = &&op_true,
        [OP_FALSE] = &&op_false,
        [OP_ADD] = &&op_add,
        [OP_SUBTRACT] = &&op_subtract,
        [OP_MULTIPLY] = &&op_multiply,
        [OP_DIVIDE] = &&op_divide,
        [OP_MODULO] = &&op_modulo,
        [OP_NEGATE] = &&op_negate,
        [OP_NOT] = &&op_not,
        [OP_EQUAL] = &&op_equal,
        [OP_NOT_EQUAL] = &&op_not_equal,
        [OP_LESS] = &&op_less,
        [OP_LESS_EQUAL] = &&op_less_equal,
        [OP_GREATER] = &&op_greater,
        [OP_GREATER_EQUAL] = &&op_greater_equal,
        [OP_PRINT] = &&op_print,
        [OP_PRINT_GLOBAL] = &&op_print_global,
        [OP_PRINT_LOCAL] = &&op_print_local,
        [OP_PRINT_CONST] = &&op_print_const,
        [OP_PRINT_CONST_LONG] = &&op_print_const_long,
        [OP_NOP] = &&op_nop,
        [OP_POP] = &&op_pop,
        [OP_GET_GLOBAL] = &&op_get_global,
        [OP_SET_GLOBAL] = &&op_set_global,
        [OP_DEFINE_GLOBAL] = &&op_define_global,
        [OP_GET_LOCAL] = &&op_get_local,
        [OP_SET_LOCAL] = &&op_set_local,
        [OP_JUMP] = &&op_jump,
        [OP_JUMP_IF_FALSE] = &&op_jump_if_false,
        [OP_JUMP_IF_FALSE_POP] = &&op_jump_if_false_pop,
        [OP_LOOP] = &&op_loop,
        [OP_INCR_GLOBAL] = &&op_incr_global,
        [OP_INCR_LOCAL] = &&op_incr_local,
        [OP_ADD_GLOBAL] = &&op_add_global,
        [OP_ADD_LOCAL] = &&op_add_local,
        [OP_INCR_GLOBAL_STMT] = &&op_incr_global_stmt,
        [OP_INCR_LOCAL_STMT] = &&op_incr_local_stmt,
        [OP_ADD_GLOBAL_STMT] = &&op_add_global_stmt,
        [OP_ADD_LOCAL_STMT] = &&op_add_local_stmt,
        [OP_SUB_GLOBAL] = &&op_sub_global,
        [OP_SUB_LOCAL] = &&op_sub_local,
        [OP_SUB_GLOBAL_STMT] = &&op_sub_global_stmt,
        [OP_SUB_LOCAL_STMT] = &&op_sub_local_stmt,
        [OP_SET_GLOBAL_MOVE] = &&op_set_global_move,
        [OP_SET_LOCAL_MOVE] = &&op_set_local_move,
        [OP_CALL] = &&op_call,
        [OP_RETURN] = &&op_return,
        [OP_HALT] = &&op_halt,
    };
#define DISPATCH() goto *dispatch_table[READ_BYTE()]
#define DISPATCH_NEXT() DISPATCH()
    DISPATCH();
#else
#define DISPATCH_NEXT() continue
#endif

#if defined(__GNUC__) && !defined(VIDA_NO_THREADED_CODE)
    op_constant: {
        uint8_t idx = READ_BYTE();
        PUSH(vida_copy_inline(consts[idx]));
        DISPATCH_NEXT();
    }
    op_constant_long: {
        uint16_t idx = READ_SHORT();
        if (VIDA_UNLIKELY(idx >= (uint16_t)chunk->const_count)) {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Bad constant index.");
            return VIDA_RUNTIME_ERROR;
        }
        PUSH(vida_copy_inline(consts[idx]));
        DISPATCH_NEXT();
    }
    op_nil: { PUSH(VIDA_NIL()); DISPATCH_NEXT(); }
    op_true: { PUSH(VIDA_BOOL(true)); DISPATCH_NEXT(); }
    op_false: { PUSH(VIDA_BOOL(false)); DISPATCH_NEXT(); }

    op_add: {
        VidaValue b = POP();
        VidaValue a = POP();
        if (VIDA_LIKELY(a.type == VAL_INT && b.type == VAL_INT)) {
            PUSH(VIDA_INT(a.as.integer + b.as.integer));
        } else if (a.type == VAL_STRING && b.type == VAL_STRING) {
            // Single malloc (old code did malloc + copy-malloc + free).
            int length = a.as.string.length + b.as.string.length;
            char *chars = (char *)malloc((size_t)length + 1);
            if (VIDA_UNLIKELY(!chars)) {
                vida_free_inline(&a); vida_free_inline(&b);
                VIDA_SYNC();
                runtime_error_at(vm, chunk, ip, "Out of memory.");
                return VIDA_RUNTIME_ERROR;
            }
            memcpy(chars, a.as.string.chars, (size_t)a.as.string.length);
            memcpy(chars + a.as.string.length, b.as.string.chars, (size_t)b.as.string.length);
            chars[length] = '\0';
            vida_free_inline(&a);
            vida_free_inline(&b);
            PUSH(vida_value_take_string(chars, length));
        } else if (a.type == VAL_DOUBLE && b.type == VAL_DOUBLE) {
            PUSH(VIDA_DOUBLE(a.as.number + b.as.number));
        } else if (a.type == VAL_INT && b.type == VAL_DOUBLE) {
            PUSH(VIDA_DOUBLE((double)a.as.integer + b.as.number));
        } else if (a.type == VAL_DOUBLE && b.type == VAL_INT) {
            PUSH(VIDA_DOUBLE(a.as.number + (double)b.as.integer));
        } else {
            vida_free_inline(&a); vida_free_inline(&b);
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Operands must be two numbers or two strings.");
            return VIDA_RUNTIME_ERROR;
        }
        DISPATCH_NEXT();
    }

#define VIDA_NUM_OP(name, op, zero_check) \
    name: { \
        VidaValue b = POP(); \
        VidaValue a = POP(); \
        if (VIDA_LIKELY(a.type == VAL_INT && b.type == VAL_INT)) { \
            if (zero_check && VIDA_UNLIKELY(b.as.integer == 0)) { \
                VIDA_SYNC(); \
                runtime_error_at(vm, chunk, ip, "Division by zero."); \
                return VIDA_RUNTIME_ERROR; \
            } \
            PUSH(VIDA_INT(a.as.integer op b.as.integer)); \
        } else if (a.type == VAL_DOUBLE && b.type == VAL_DOUBLE) { \
            PUSH(VIDA_DOUBLE(a.as.number op b.as.number)); \
        } else if (a.type == VAL_INT && b.type == VAL_DOUBLE) { \
            PUSH(VIDA_DOUBLE((double)a.as.integer op b.as.number)); \
        } else if (a.type == VAL_DOUBLE && b.type == VAL_INT) { \
            PUSH(VIDA_DOUBLE(a.as.number op (double)b.as.integer)); \
        } else { \
            vida_free_inline(&a); vida_free_inline(&b); \
            VIDA_SYNC(); \
            runtime_error_at(vm, chunk, ip, "Operands must be numbers."); \
            return VIDA_RUNTIME_ERROR; \
        } \
        DISPATCH_NEXT(); \
    }
    VIDA_NUM_OP(op_subtract, -, 0)
    VIDA_NUM_OP(op_multiply, *, 0)
    VIDA_NUM_OP(op_divide, /, 1)

    op_modulo: {
        VidaValue b = POP();
        VidaValue a = POP();
        if (VIDA_LIKELY(a.type == VAL_INT && b.type == VAL_INT)) {
            if (VIDA_UNLIKELY(b.as.integer == 0)) {
                VIDA_SYNC();
                runtime_error_at(vm, chunk, ip, "Division by zero.");
                return VIDA_RUNTIME_ERROR;
            }
            PUSH(VIDA_INT(a.as.integer % b.as.integer));
        } else {
            vida_free_inline(&a); vida_free_inline(&b);
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Modulo requires integer operands.");
            return VIDA_RUNTIME_ERROR;
        }
        DISPATCH_NEXT();
    }
    op_negate: {
        VidaValue a = POP();
        if (VIDA_LIKELY(a.type == VAL_INT)) {
            PUSH(VIDA_INT(-a.as.integer));
        } else if (a.type == VAL_DOUBLE) {
            PUSH(VIDA_DOUBLE(-a.as.number));
        } else {
            vida_free_inline(&a);
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Operand must be a number.");
            return VIDA_RUNTIME_ERROR;
        }
        DISPATCH_NEXT();
    }
    op_not: {
        VidaValue a = POP();
        bool r = vida_is_falsey_inline(a);
        vida_free_inline(&a);
        PUSH(VIDA_BOOL(r));
        DISPATCH_NEXT();
    }
    op_equal: {
        VidaValue b = POP();
        VidaValue a = POP();
        bool r = vida_values_equal(a, b);
        vida_free_inline(&a); vida_free_inline(&b);
        PUSH(VIDA_BOOL(r));
        DISPATCH_NEXT();
    }
    op_not_equal: {
        VidaValue b = POP();
        VidaValue a = POP();
        bool r = !vida_values_equal(a, b);
        vida_free_inline(&a); vida_free_inline(&b);
        PUSH(VIDA_BOOL(r));
        DISPATCH_NEXT();
    }

#define VIDA_CMP_OP(name, op) \
    name: { \
        VidaValue b = POP(); \
        VidaValue a = POP(); \
        bool r; \
        if (VIDA_LIKELY(a.type == VAL_INT && b.type == VAL_INT)) { \
            r = (a.as.integer op b.as.integer); \
        } else if (a.type == VAL_DOUBLE && b.type == VAL_DOUBLE) { \
            r = (a.as.number op b.as.number); \
        } else if (a.type == VAL_INT && b.type == VAL_DOUBLE) { \
            r = ((double)a.as.integer op b.as.number); \
        } else if (a.type == VAL_DOUBLE && b.type == VAL_INT) { \
            r = (a.as.number op (double)b.as.integer); \
        } else { \
            vida_free_inline(&a); vida_free_inline(&b); \
            VIDA_SYNC(); \
            runtime_error_at(vm, chunk, ip, "Operands must be numbers."); \
            return VIDA_RUNTIME_ERROR; \
        } \
        PUSH(VIDA_BOOL(r)); \
        DISPATCH_NEXT(); \
    }
    VIDA_CMP_OP(op_less, <)
    VIDA_CMP_OP(op_less_equal, <=)
    VIDA_CMP_OP(op_greater, >)
    VIDA_CMP_OP(op_greater_equal, >=)

    op_print: {
        VidaValue a = POP();
        vida_value_print(a);
        putchar('\n');
        vida_free_inline(&a);
        DISPATCH_NEXT();
    }
    op_print_global: {
        uint16_t slot = READ_SHORT();
        if (VIDA_UNLIKELY(slot >= (uint16_t)nglobals)) {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Undefined global variable.");
            return VIDA_RUNTIME_ERROR;
        }
        vida_value_print(globals[slot]);
        putchar('\n');
        DISPATCH_NEXT();
    }
    op_print_local: {
        uint8_t slot = READ_BYTE();
        vida_value_print(slots[slot]);
        putchar('\n');
        DISPATCH_NEXT();
    }
    op_print_const: {
        uint8_t idx = READ_BYTE();
        if (VIDA_UNLIKELY(idx >= chunk->const_count)) {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Bad constant index.");
            return VIDA_RUNTIME_ERROR;
        }
        vida_value_print(consts[idx]);
        putchar('\n');
        DISPATCH_NEXT();
    }
    op_print_const_long: {
        uint16_t idx = READ_SHORT();
        if (VIDA_UNLIKELY(idx >= (uint16_t)chunk->const_count)) {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Bad constant index.");
            return VIDA_RUNTIME_ERROR;
        }
        vida_value_print(consts[idx]);
        putchar('\n');
        DISPATCH_NEXT();
    }
    op_nop: {
        DISPATCH_NEXT();
    }
    op_pop: {
        VidaValue v = POP();
        vida_free_inline(&v);
        DISPATCH_NEXT();
    }
    op_get_global: {
        uint16_t slot = READ_SHORT();
        if (VIDA_UNLIKELY(slot >= (uint16_t)nglobals)) {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Undefined global variable.");
            return VIDA_RUNTIME_ERROR;
        }
        PUSH(vida_copy_inline(globals[slot]));
        DISPATCH_NEXT();
    }
    op_set_global: {
        uint16_t slot = READ_SHORT();
        if (VIDA_UNLIKELY(slot >= VIDA_GLOBALS_MAX)) {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Too many globals.");
            return VIDA_RUNTIME_ERROR;
        }
        VidaValue val = PEEK(0);
        if (slot < nglobals) {
            vida_free_inline(&globals[slot]);
            globals[slot] = vida_copy_inline(val);
        } else if (slot == nglobals) {
            globals[nglobals++] = vida_copy_inline(val);
        } else {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Undefined global variable.");
            return VIDA_RUNTIME_ERROR;
        }
        DISPATCH_NEXT();
    }
    op_define_global: {
        uint16_t slot = READ_SHORT();
        if (VIDA_UNLIKELY(slot >= VIDA_GLOBALS_MAX)) {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Too many globals.");
            return VIDA_RUNTIME_ERROR;
        }
        VidaValue val = POP(); // move, no copy
        if (slot < nglobals) {
            vida_free_inline(&globals[slot]);
            globals[slot] = val;
        } else if (slot == nglobals) {
            globals[nglobals++] = val;
        } else {
            vida_free_inline(&val);
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Undefined global variable.");
            return VIDA_RUNTIME_ERROR;
        }
        DISPATCH_NEXT();
    }
    op_get_local: {
        uint8_t slot = READ_BYTE();
        PUSH(vida_copy_inline(slots[slot]));
        DISPATCH_NEXT();
    }
    op_set_local: {
        uint8_t slot = READ_BYTE();
        vida_free_inline(&slots[slot]);
        slots[slot] = vida_copy_inline(PEEK(0));
        DISPATCH_NEXT();
    }
    op_jump: {
        uint16_t off = READ_SHORT();
        ip += off;
        DISPATCH_NEXT();
    }
    op_jump_if_false: {
        uint16_t off = READ_SHORT();
        if (vida_is_falsey_inline(PEEK(0))) ip += off;
        DISPATCH_NEXT();
    }
    op_jump_if_false_pop: {
        uint16_t off = READ_SHORT();
        VidaValue c = POP();
        bool f = vida_is_falsey_inline(c);
        vida_free_inline(&c);
        if (f) ip += off;
        DISPATCH_NEXT();
    }
    op_loop: {
        uint16_t off = READ_SHORT();
        ip -= off;
        DISPATCH_NEXT();
    }
    op_incr_global: {
        uint16_t slot = READ_SHORT();
        int16_t delta = READ_I16();
        if (VIDA_UNLIKELY(slot >= (uint16_t)nglobals)) {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Undefined global variable.");
            return VIDA_RUNTIME_ERROR;
        }
        VidaValue *g = &globals[slot];
        if (VIDA_LIKELY(g->type == VAL_INT)) {
            g->as.integer += delta;
            PUSH(VIDA_INT(g->as.integer));
        } else if (g->type == VAL_DOUBLE) {
            g->as.number += (double)delta;
            PUSH(VIDA_DOUBLE(g->as.number));
        } else {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Operand must be a number.");
            return VIDA_RUNTIME_ERROR;
        }
        DISPATCH_NEXT();
    }
    op_incr_local: {
        uint8_t slot = READ_BYTE();
        int16_t delta = READ_I16();
        VidaValue *g = &slots[slot];
        if (VIDA_LIKELY(g->type == VAL_INT)) {
            g->as.integer += delta;
            PUSH(VIDA_INT(g->as.integer));
        } else if (g->type == VAL_DOUBLE) {
            g->as.number += (double)delta;
            PUSH(VIDA_DOUBLE(g->as.number));
        } else {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Operand must be a number.");
            return VIDA_RUNTIME_ERROR;
        }
        DISPATCH_NEXT();
    }
    op_add_global: {
        uint16_t slot = READ_SHORT();
        if (VIDA_UNLIKELY(slot >= (uint16_t)nglobals)) {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Undefined global variable.");
            return VIDA_RUNTIME_ERROR;
        }
        VidaValue rhs = POP();
        VidaValue *g = &globals[slot];
        if (VIDA_LIKELY(g->type == VAL_INT && rhs.type == VAL_INT)) {
            g->as.integer += rhs.as.integer;
            PUSH(VIDA_INT(g->as.integer));
        } else if (g->type == VAL_STRING && rhs.type == VAL_STRING) {
            if (VIDA_UNLIKELY(!vida_append_inline(g, &rhs))) {
                vida_free_inline(&rhs);
                VIDA_SYNC();
                runtime_error_at(vm, chunk, ip, "Out of memory.");
                return VIDA_RUNTIME_ERROR;
            }
            vida_free_inline(&rhs);
            PUSH(vida_copy_inline(*g));
        } else if (g->type == VAL_DOUBLE && rhs.type == VAL_DOUBLE) {
            g->as.number += rhs.as.number;
            PUSH(VIDA_DOUBLE(g->as.number));
        } else if (g->type == VAL_INT && rhs.type == VAL_DOUBLE) {
            g->type = VAL_DOUBLE;
            g->as.number = (double)g->as.integer + rhs.as.number;
            PUSH(VIDA_DOUBLE(g->as.number));
        } else if (g->type == VAL_DOUBLE && rhs.type == VAL_INT) {
            g->as.number += (double)rhs.as.integer;
            PUSH(VIDA_DOUBLE(g->as.number));
        } else {
            vida_free_inline(&rhs);
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Operands must be two numbers or two strings.");
            return VIDA_RUNTIME_ERROR;
        }
        DISPATCH_NEXT();
    }
    op_add_local: {
        uint8_t slot = READ_BYTE();
        VidaValue rhs = POP();
        VidaValue *g = &slots[slot];
        if (VIDA_LIKELY(g->type == VAL_INT && rhs.type == VAL_INT)) {
            g->as.integer += rhs.as.integer;
            PUSH(VIDA_INT(g->as.integer));
        } else if (g->type == VAL_STRING && rhs.type == VAL_STRING) {
            if (VIDA_UNLIKELY(!vida_append_inline(g, &rhs))) {
                vida_free_inline(&rhs);
                VIDA_SYNC();
                runtime_error_at(vm, chunk, ip, "Out of memory.");
                return VIDA_RUNTIME_ERROR;
            }
            vida_free_inline(&rhs);
            PUSH(vida_copy_inline(*g));
        } else if (g->type == VAL_DOUBLE && rhs.type == VAL_DOUBLE) {
            g->as.number += rhs.as.number;
            PUSH(VIDA_DOUBLE(g->as.number));
        } else if (g->type == VAL_INT && rhs.type == VAL_DOUBLE) {
            g->type = VAL_DOUBLE;
            g->as.number = (double)g->as.integer + rhs.as.number;
            PUSH(VIDA_DOUBLE(g->as.number));
        } else if (g->type == VAL_DOUBLE && rhs.type == VAL_INT) {
            g->as.number += (double)rhs.as.integer;
            PUSH(VIDA_DOUBLE(g->as.number));
        } else {
            vida_free_inline(&rhs);
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Operands must be two numbers or two strings.");
            return VIDA_RUNTIME_ERROR;
        }
        DISPATCH_NEXT();
    }
    op_incr_global_stmt: {
        uint16_t slot = READ_SHORT();
        int16_t delta = READ_I16();
        if (VIDA_UNLIKELY(slot >= (uint16_t)nglobals)) {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Undefined global variable.");
            return VIDA_RUNTIME_ERROR;
        }
        VidaValue *g = &globals[slot];
        if (VIDA_LIKELY(g->type == VAL_INT)) {
            g->as.integer += delta;
        } else if (g->type == VAL_DOUBLE) {
            g->as.number += (double)delta;
        } else {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Operand must be a number.");
            return VIDA_RUNTIME_ERROR;
        }
        DISPATCH_NEXT();
    }
    op_incr_local_stmt: {
        uint8_t slot = READ_BYTE();
        int16_t delta = READ_I16();
        VidaValue *g = &slots[slot];
        if (VIDA_LIKELY(g->type == VAL_INT)) {
            g->as.integer += delta;
        } else if (g->type == VAL_DOUBLE) {
            g->as.number += (double)delta;
        } else {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Operand must be a number.");
            return VIDA_RUNTIME_ERROR;
        }
        DISPATCH_NEXT();
    }
    op_add_global_stmt: {
        uint16_t slot = READ_SHORT();
        if (VIDA_UNLIKELY(slot >= (uint16_t)nglobals)) {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Undefined global variable.");
            return VIDA_RUNTIME_ERROR;
        }
        VidaValue rhs = POP();
        VidaValue *g = &globals[slot];
        if (VIDA_LIKELY(g->type == VAL_INT && rhs.type == VAL_INT)) {
            g->as.integer += rhs.as.integer;
        } else if (g->type == VAL_STRING && rhs.type == VAL_STRING) {
            if (VIDA_UNLIKELY(!vida_append_inline(g, &rhs))) {
                vida_free_inline(&rhs);
                VIDA_SYNC();
                runtime_error_at(vm, chunk, ip, "Out of memory.");
                return VIDA_RUNTIME_ERROR;
            }
            vida_free_inline(&rhs);
        } else if (g->type == VAL_DOUBLE && rhs.type == VAL_DOUBLE) {
            g->as.number += rhs.as.number;
        } else if (g->type == VAL_INT && rhs.type == VAL_DOUBLE) {
            g->type = VAL_DOUBLE;
            g->as.number = (double)g->as.integer + rhs.as.number;
        } else if (g->type == VAL_DOUBLE && rhs.type == VAL_INT) {
            g->as.number += (double)rhs.as.integer;
        } else {
            vida_free_inline(&rhs);
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Operands must be two numbers or two strings.");
            return VIDA_RUNTIME_ERROR;
        }
        DISPATCH_NEXT();
    }
    op_add_local_stmt: {
        uint8_t slot = READ_BYTE();
        VidaValue rhs = POP();
        VidaValue *g = &slots[slot];
        if (VIDA_LIKELY(g->type == VAL_INT && rhs.type == VAL_INT)) {
            g->as.integer += rhs.as.integer;
        } else if (g->type == VAL_STRING && rhs.type == VAL_STRING) {
            if (VIDA_UNLIKELY(!vida_append_inline(g, &rhs))) {
                vida_free_inline(&rhs);
                VIDA_SYNC();
                runtime_error_at(vm, chunk, ip, "Out of memory.");
                return VIDA_RUNTIME_ERROR;
            }
            vida_free_inline(&rhs);
        } else if (g->type == VAL_DOUBLE && rhs.type == VAL_DOUBLE) {
            g->as.number += rhs.as.number;
        } else if (g->type == VAL_INT && rhs.type == VAL_DOUBLE) {
            g->type = VAL_DOUBLE;
            g->as.number = (double)g->as.integer + rhs.as.number;
        } else if (g->type == VAL_DOUBLE && rhs.type == VAL_INT) {
            g->as.number += (double)rhs.as.integer;
        } else {
            vida_free_inline(&rhs);
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Operands must be two numbers or two strings.");
            return VIDA_RUNTIME_ERROR;
        }
        DISPATCH_NEXT();
    }
    op_sub_global: {
        uint16_t slot = READ_SHORT();
        if (VIDA_UNLIKELY(slot >= (uint16_t)nglobals)) {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Undefined global variable.");
            return VIDA_RUNTIME_ERROR;
        }
        VidaValue rhs = POP();
        VidaValue *g = &globals[slot];
        if (VIDA_LIKELY(g->type == VAL_INT && rhs.type == VAL_INT)) {
            g->as.integer -= rhs.as.integer;
            PUSH(VIDA_INT(g->as.integer));
        } else if (g->type == VAL_DOUBLE && rhs.type == VAL_DOUBLE) {
            g->as.number -= rhs.as.number;
            PUSH(VIDA_DOUBLE(g->as.number));
        } else if (g->type == VAL_INT && rhs.type == VAL_DOUBLE) {
            g->type = VAL_DOUBLE;
            g->as.number = (double)g->as.integer - rhs.as.number;
            PUSH(VIDA_DOUBLE(g->as.number));
        } else if (g->type == VAL_DOUBLE && rhs.type == VAL_INT) {
            g->as.number -= (double)rhs.as.integer;
            PUSH(VIDA_DOUBLE(g->as.number));
        } else {
            vida_free_inline(&rhs);
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Operands must be numbers.");
            return VIDA_RUNTIME_ERROR;
        }
        DISPATCH_NEXT();
    }
    op_sub_local: {
        uint8_t slot = READ_BYTE();
        VidaValue rhs = POP();
        VidaValue *g = &slots[slot];
        if (VIDA_LIKELY(g->type == VAL_INT && rhs.type == VAL_INT)) {
            g->as.integer -= rhs.as.integer;
            PUSH(VIDA_INT(g->as.integer));
        } else if (g->type == VAL_DOUBLE && rhs.type == VAL_DOUBLE) {
            g->as.number -= rhs.as.number;
            PUSH(VIDA_DOUBLE(g->as.number));
        } else if (g->type == VAL_INT && rhs.type == VAL_DOUBLE) {
            g->type = VAL_DOUBLE;
            g->as.number = (double)g->as.integer - rhs.as.number;
            PUSH(VIDA_DOUBLE(g->as.number));
        } else if (g->type == VAL_DOUBLE && rhs.type == VAL_INT) {
            g->as.number -= (double)rhs.as.integer;
            PUSH(VIDA_DOUBLE(g->as.number));
        } else {
            vida_free_inline(&rhs);
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Operands must be numbers.");
            return VIDA_RUNTIME_ERROR;
        }
        DISPATCH_NEXT();
    }
    op_sub_global_stmt: {
        uint16_t slot = READ_SHORT();
        if (VIDA_UNLIKELY(slot >= (uint16_t)nglobals)) {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Undefined global variable.");
            return VIDA_RUNTIME_ERROR;
        }
        VidaValue rhs = POP();
        VidaValue *g = &globals[slot];
        if (VIDA_LIKELY(g->type == VAL_INT && rhs.type == VAL_INT)) {
            g->as.integer -= rhs.as.integer;
        } else if (g->type == VAL_DOUBLE && rhs.type == VAL_DOUBLE) {
            g->as.number -= rhs.as.number;
        } else if (g->type == VAL_INT && rhs.type == VAL_DOUBLE) {
            g->type = VAL_DOUBLE;
            g->as.number = (double)g->as.integer - rhs.as.number;
        } else if (g->type == VAL_DOUBLE && rhs.type == VAL_INT) {
            g->as.number -= (double)rhs.as.integer;
        } else {
            vida_free_inline(&rhs);
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Operands must be numbers.");
            return VIDA_RUNTIME_ERROR;
        }
        DISPATCH_NEXT();
    }
    op_sub_local_stmt: {
        uint8_t slot = READ_BYTE();
        VidaValue rhs = POP();
        VidaValue *g = &slots[slot];
        if (VIDA_LIKELY(g->type == VAL_INT && rhs.type == VAL_INT)) {
            g->as.integer -= rhs.as.integer;
        } else if (g->type == VAL_DOUBLE && rhs.type == VAL_DOUBLE) {
            g->as.number -= rhs.as.number;
        } else if (g->type == VAL_INT && rhs.type == VAL_DOUBLE) {
            g->type = VAL_DOUBLE;
            g->as.number = (double)g->as.integer - rhs.as.number;
        } else if (g->type == VAL_DOUBLE && rhs.type == VAL_INT) {
            g->as.number -= (double)rhs.as.integer;
        } else {
            vida_free_inline(&rhs);
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Operands must be numbers.");
            return VIDA_RUNTIME_ERROR;
        }
        DISPATCH_NEXT();
    }
    op_set_global_move: {
        uint16_t slot = READ_SHORT();
        if (VIDA_UNLIKELY(slot >= VIDA_GLOBALS_MAX)) {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Too many globals.");
            return VIDA_RUNTIME_ERROR;
        }
        VidaValue val = POP(); // move: no copy, no leftover on stack
        if (slot < nglobals) {
            vida_free_inline(&globals[slot]);
            globals[slot] = val;
        } else if (slot == nglobals) {
            globals[nglobals++] = val;
        } else {
            vida_free_inline(&val);
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Undefined global variable.");
            return VIDA_RUNTIME_ERROR;
        }
        DISPATCH_NEXT();
    }
    op_set_local_move: {
        uint8_t slot = READ_BYTE();
        VidaValue val = POP(); // move into local slot
        vida_free_inline(&slots[slot]);
        slots[slot] = val;
        DISPATCH_NEXT();
    }
    op_call: {
        uint8_t argc = READ_BYTE();
        if (VIDA_UNLIKELY(top < (int)argc + 1)) {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Stack underflow in call.");
            return VIDA_RUNTIME_ERROR;
        }
        VidaValue callee = stack[top - argc - 1]; // shallow borrow
        if (callee.type == VAL_NATIVE) {
            VidaValue result;
            VidaNativeFn fn = callee.as.native.fn;
            bool ok = fn(argc, &stack[top - argc], &result);
            // Free callee + arg copies (natives borrow; result owns its value).
            for (int i = top - argc - 1; i < top; i++) vida_free_inline(&stack[i]);
            top -= argc + 1;
            if (VIDA_UNLIKELY(!ok)) {
                VIDA_SYNC();
                runtime_error_at(vm, chunk, ip, "Bad arguments to built-in function.");
                return VIDA_RUNTIME_ERROR;
            }
            PUSH(result);
            DISPATCH_NEXT();
        }
        if (VIDA_UNLIKELY(callee.type != VAL_FUNC)) {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Can only call functions.");
            return VIDA_RUNTIME_ERROR;
        }
        VidaFunc *fn = callee.as.func;
        if (VIDA_UNLIKELY(argc != fn->arity)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Expected %d args but got %d.", fn->arity, argc);
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, msg);
            return VIDA_RUNTIME_ERROR;
        }
        if (VIDA_UNLIKELY(vm->frame_count >= VIDA_FRAMES_MAX)) {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Call stack overflow.");
            return VIDA_RUNTIME_ERROR;
        }
        if (VIDA_UNLIKELY(top + VIDA_FRAME_STACK >= VIDA_STACK_MAX)) {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Stack overflow.");
            return VIDA_RUNTIME_ERROR;
        }
        frame->ip = ip; // save caller position
        vm->stack_top = top;
        frame = &vm->frames[vm->frame_count++];
        frame->func = fn; // borrowed: slots[0] owns the ref
        frame->slots = &stack[top - argc - 1];
        frame->ip = fn->chunk.code; // start of callee bytecode
        VIDA_LOAD_FRAME();
        DISPATCH_NEXT();
    }
    op_return: {
        if (VIDA_UNLIKELY(top <= (int)(frame->slots - stack))) {
            VIDA_SYNC();
            runtime_error_at(vm, chunk, ip, "Stack underflow in return.");
            return VIDA_RUNTIME_ERROR;
        }
        VidaValue res = POP();
        VidaValue *base = frame->slots;
        for (VidaValue *p = base; p < &stack[top]; p++) vida_free_inline(p);
        top = (int)(base - stack);
        if (--vm->frame_count == 0) {
            vida_free_inline(&res); // script result is unused
            VIDA_SYNC();
            return VIDA_OK;
        }
        frame = &vm->frames[vm->frame_count - 1];
        VIDA_LOAD_FRAME();
        PUSH(res); // transfer ownership, no copy
        DISPATCH_NEXT();
    }
    op_halt: {
        VIDA_SYNC();
        return VIDA_OK;
    }
#else
    // Portable switch fallback (MSVC / unknown compilers).
    for (;;) {
        uint8_t instruction = READ_BYTE();
        switch (instruction) {
            case OP_CONSTANT: {
                uint8_t idx = READ_BYTE();
                VidaValue v = consts[idx];
                if (VIDA_UNLIKELY(top >= VIDA_STACK_MAX)) {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Stack overflow");
                    return VIDA_RUNTIME_ERROR;
                }
                stack[top++] = vida_copy_inline(v);
                break;
            }
            case OP_CONSTANT_LONG: {
                uint16_t idx = READ_SHORT();
                if (VIDA_UNLIKELY(idx >= (uint16_t)chunk->const_count)) {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Bad constant index.");
                    return VIDA_RUNTIME_ERROR;
                }
                if (VIDA_UNLIKELY(top >= VIDA_STACK_MAX)) {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Stack overflow");
                    return VIDA_RUNTIME_ERROR;
                }
                stack[top++] = vida_copy_inline(consts[idx]);
                break;
            }
            case OP_NIL: stack[top++] = VIDA_NIL(); break;
            case OP_TRUE: stack[top++] = VIDA_BOOL(true); break;
            case OP_FALSE: stack[top++] = VIDA_BOOL(false); break;
            case OP_ADD: {
                VidaValue b = stack[--top];
                VidaValue a = stack[--top];
                if (VIDA_LIKELY(a.type == VAL_INT && b.type == VAL_INT)) {
                    stack[top++] = VIDA_INT(a.as.integer + b.as.integer);
                } else if (a.type == VAL_STRING && b.type == VAL_STRING) {
                    int length = a.as.string.length + b.as.string.length;
                    char *chars = (char *)malloc((size_t)length + 1);
                    if (VIDA_UNLIKELY(!chars)) {
                        vida_free_inline(&a); vida_free_inline(&b);
                        VIDA_SYNC();
                        runtime_error_at(vm, chunk, ip, "Out of memory.");
                        return VIDA_RUNTIME_ERROR;
                    }
                    memcpy(chars, a.as.string.chars, (size_t)a.as.string.length);
                    memcpy(chars + a.as.string.length, b.as.string.chars, (size_t)b.as.string.length);
                    chars[length] = '\0';
                    vida_free_inline(&a); vida_free_inline(&b);
                    stack[top++] = vida_value_take_string(chars, length);
                } else if (a.type == VAL_DOUBLE && b.type == VAL_DOUBLE) {
                    stack[top++] = VIDA_DOUBLE(a.as.number + b.as.number);
                } else if (a.type == VAL_INT && b.type == VAL_DOUBLE) {
                    stack[top++] = VIDA_DOUBLE((double)a.as.integer + b.as.number);
                } else if (a.type == VAL_DOUBLE && b.type == VAL_INT) {
                    stack[top++] = VIDA_DOUBLE(a.as.number + (double)b.as.integer);
                } else {
                    vida_free_inline(&a); vida_free_inline(&b);
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Operands must be two numbers or two strings.");
                    return VIDA_RUNTIME_ERROR;
                }
                break;
            }
            case OP_SUBTRACT:
            case OP_MULTIPLY:
            case OP_DIVIDE: {
                VidaValue b = stack[--top];
                VidaValue a = stack[--top];
                if (VIDA_LIKELY(a.type == VAL_INT && b.type == VAL_INT)) {
                    if (instruction == OP_DIVIDE && VIDA_UNLIKELY(b.as.integer == 0)) {
                        VIDA_SYNC();
                        runtime_error_at(vm, chunk, ip, "Division by zero.");
                        return VIDA_RUNTIME_ERROR;
                    }
                    int64_t r = instruction == OP_SUBTRACT ? a.as.integer - b.as.integer
                        : instruction == OP_MULTIPLY ? a.as.integer * b.as.integer
                        : a.as.integer / b.as.integer;
                    stack[top++] = VIDA_INT(r);
                } else if (a.type == VAL_DOUBLE && b.type == VAL_DOUBLE) {
                    double r = instruction == OP_SUBTRACT ? a.as.number - b.as.number
                        : instruction == OP_MULTIPLY ? a.as.number * b.as.number
                        : a.as.number / b.as.number;
                    stack[top++] = VIDA_DOUBLE(r);
                } else if (a.type == VAL_INT && b.type == VAL_DOUBLE) {
                    double r = instruction == OP_SUBTRACT ? (double)a.as.integer - b.as.number
                        : instruction == OP_MULTIPLY ? (double)a.as.integer * b.as.number
                        : (double)a.as.integer / b.as.number;
                    stack[top++] = VIDA_DOUBLE(r);
                } else if (a.type == VAL_DOUBLE && b.type == VAL_INT) {
                    double r = instruction == OP_SUBTRACT ? a.as.number - (double)b.as.integer
                        : instruction == OP_MULTIPLY ? a.as.number * (double)b.as.integer
                        : a.as.number / (double)b.as.integer;
                    stack[top++] = VIDA_DOUBLE(r);
                } else {
                    vida_free_inline(&a); vida_free_inline(&b);
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Operands must be numbers.");
                    return VIDA_RUNTIME_ERROR;
                }
                break;
            }
            case OP_MODULO: {
                VidaValue b = stack[--top];
                VidaValue a = stack[--top];
                if (VIDA_LIKELY(a.type == VAL_INT && b.type == VAL_INT)) {
                    if (VIDA_UNLIKELY(b.as.integer == 0)) {
                        VIDA_SYNC();
                        runtime_error_at(vm, chunk, ip, "Division by zero.");
                        return VIDA_RUNTIME_ERROR;
                    }
                    stack[top++] = VIDA_INT(a.as.integer % b.as.integer);
                } else {
                    vida_free_inline(&a); vida_free_inline(&b);
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Modulo requires integer operands.");
                    return VIDA_RUNTIME_ERROR;
                }
                break;
            }
            case OP_NEGATE: {
                VidaValue a = stack[--top];
                if (VIDA_LIKELY(a.type == VAL_INT)) stack[top++] = VIDA_INT(-a.as.integer);
                else if (a.type == VAL_DOUBLE) stack[top++] = VIDA_DOUBLE(-a.as.number);
                else {
                    vida_free_inline(&a);
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Operand must be a number.");
                    return VIDA_RUNTIME_ERROR;
                }
                break;
            }
            case OP_NOT: {
                VidaValue a = stack[--top];
                bool r = vida_is_falsey_inline(a);
                vida_free_inline(&a);
                stack[top++] = VIDA_BOOL(r);
                break;
            }
            case OP_EQUAL: {
                VidaValue b = stack[--top];
                VidaValue a = stack[--top];
                bool r = vida_values_equal(a, b);
                vida_free_inline(&a); vida_free_inline(&b);
                stack[top++] = VIDA_BOOL(r);
                break;
            }
            case OP_NOT_EQUAL: {
                VidaValue b = stack[--top];
                VidaValue a = stack[--top];
                bool r = !vida_values_equal(a, b);
                vida_free_inline(&a); vida_free_inline(&b);
                stack[top++] = VIDA_BOOL(r);
                break;
            }
            case OP_LESS:
            case OP_LESS_EQUAL:
            case OP_GREATER:
            case OP_GREATER_EQUAL: {
                VidaValue b = stack[--top];
                VidaValue a = stack[--top];
                bool r;
                if (VIDA_LIKELY(a.type == VAL_INT && b.type == VAL_INT)) {
                    if (instruction == OP_LESS) r = a.as.integer < b.as.integer;
                    else if (instruction == OP_LESS_EQUAL) r = a.as.integer <= b.as.integer;
                    else if (instruction == OP_GREATER) r = a.as.integer > b.as.integer;
                    else r = a.as.integer >= b.as.integer;
                } else if (a.type == VAL_DOUBLE && b.type == VAL_DOUBLE) {
                    if (instruction == OP_LESS) r = a.as.number < b.as.number;
                    else if (instruction == OP_LESS_EQUAL) r = a.as.number <= b.as.number;
                    else if (instruction == OP_GREATER) r = a.as.number > b.as.number;
                    else r = a.as.number >= b.as.number;
                } else if (a.type == VAL_INT && b.type == VAL_DOUBLE) {
                    double x = (double)a.as.integer;
                    if (instruction == OP_LESS) r = x < b.as.number;
                    else if (instruction == OP_LESS_EQUAL) r = x <= b.as.number;
                    else if (instruction == OP_GREATER) r = x > b.as.number;
                    else r = x >= b.as.number;
                } else if (a.type == VAL_DOUBLE && b.type == VAL_INT) {
                    double y = (double)b.as.integer;
                    if (instruction == OP_LESS) r = a.as.number < y;
                    else if (instruction == OP_LESS_EQUAL) r = a.as.number <= y;
                    else if (instruction == OP_GREATER) r = a.as.number > y;
                    else r = a.as.number >= y;
                } else {
                    vida_free_inline(&a); vida_free_inline(&b);
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Operands must be numbers.");
                    return VIDA_RUNTIME_ERROR;
                }
                stack[top++] = VIDA_BOOL(r);
                break;
            }
            case OP_PRINT: {
                VidaValue a = stack[--top];
                vida_value_print(a);
                putchar('\n');
                vida_free_inline(&a);
                break;
            }
            case OP_PRINT_GLOBAL: {
                uint16_t slot = READ_SHORT();
                if (VIDA_UNLIKELY(slot >= (uint16_t)nglobals)) {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Undefined global variable.");
                    return VIDA_RUNTIME_ERROR;
                }
                vida_value_print(globals[slot]);
                putchar('\n');
                break;
            }
            case OP_PRINT_LOCAL: {
                uint8_t slot = READ_BYTE();
                vida_value_print(slots[slot]);
                putchar('\n');
                break;
            }
            case OP_PRINT_CONST: {
                uint8_t idx = READ_BYTE();
                if (VIDA_UNLIKELY(idx >= chunk->const_count)) {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Bad constant index.");
                    return VIDA_RUNTIME_ERROR;
                }
                vida_value_print(consts[idx]);
                putchar('\n');
                break;
            }
            case OP_PRINT_CONST_LONG: {
                uint16_t idx = READ_SHORT();
                if (VIDA_UNLIKELY(idx >= (uint16_t)chunk->const_count)) {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Bad constant index.");
                    return VIDA_RUNTIME_ERROR;
                }
                vida_value_print(consts[idx]);
                putchar('\n');
                break;
            }
            case OP_NOP: {
                break;
            }
            case OP_POP: {
                VidaValue v = stack[--top];
                vida_free_inline(&v);
                break;
            }
            case OP_GET_GLOBAL: {
                uint16_t slot = READ_SHORT();
                if (VIDA_UNLIKELY(slot >= (uint16_t)nglobals)) {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Undefined global variable.");
                    return VIDA_RUNTIME_ERROR;
                }
                stack[top++] = vida_copy_inline(globals[slot]);
                break;
            }
            case OP_SET_GLOBAL: {
                uint16_t slot = READ_SHORT();
                VidaValue val = stack[top - 1];
                if (slot < nglobals) {
                    vida_free_inline(&globals[slot]);
                    globals[slot] = vida_copy_inline(val);
                } else if (slot == nglobals && nglobals < VIDA_GLOBALS_MAX) {
                    globals[nglobals++] = vida_copy_inline(val);
                } else {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Undefined global variable.");
                    return VIDA_RUNTIME_ERROR;
                }
                break;
            }
            case OP_DEFINE_GLOBAL: {
                uint16_t slot = READ_SHORT();
                VidaValue val = stack[--top];
                if (slot < nglobals) {
                    vida_free_inline(&globals[slot]);
                    globals[slot] = val;
                } else if (slot == nglobals && nglobals < VIDA_GLOBALS_MAX) {
                    globals[nglobals++] = val;
                } else {
                    vida_free_inline(&val);
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Undefined global variable.");
                    return VIDA_RUNTIME_ERROR;
                }
                break;
            }
            case OP_GET_LOCAL: {
                uint8_t slot = READ_BYTE();
                stack[top++] = vida_copy_inline(slots[slot]);
                break;
            }
            case OP_SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                vida_free_inline(&slots[slot]);
                slots[slot] = vida_copy_inline(stack[top - 1]);
                break;
            }
            case OP_JUMP: {
                uint16_t off = READ_SHORT();
                ip += off;
                break;
            }
            case OP_JUMP_IF_FALSE: {
                uint16_t off = READ_SHORT();
                if (vida_is_falsey_inline(stack[top - 1])) ip += off;
                break;
            }
            case OP_JUMP_IF_FALSE_POP: {
                uint16_t off = READ_SHORT();
                VidaValue c = stack[--top];
                bool f = vida_is_falsey_inline(c);
                vida_free_inline(&c);
                if (f) ip += off;
                break;
            }
            case OP_LOOP: {
                uint16_t off = READ_SHORT();
                ip -= off;
                break;
            }
            case OP_INCR_GLOBAL: {
                uint16_t slot = READ_SHORT();
                int16_t delta = READ_I16();
                if (VIDA_UNLIKELY(slot >= (uint16_t)nglobals)) {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Undefined global variable.");
                    return VIDA_RUNTIME_ERROR;
                }
                VidaValue *g = &globals[slot];
                if (VIDA_LIKELY(g->type == VAL_INT)) { g->as.integer += delta; stack[top++] = VIDA_INT(g->as.integer); }
                else if (g->type == VAL_DOUBLE) { g->as.number += (double)delta; stack[top++] = VIDA_DOUBLE(g->as.number); }
                else {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Operand must be a number.");
                    return VIDA_RUNTIME_ERROR;
                }
                break;
            }
            case OP_INCR_LOCAL: {
                uint8_t slot = READ_BYTE();
                int16_t delta = READ_I16();
                VidaValue *g = &slots[slot];
                if (VIDA_LIKELY(g->type == VAL_INT)) { g->as.integer += delta; stack[top++] = VIDA_INT(g->as.integer); }
                else if (g->type == VAL_DOUBLE) { g->as.number += (double)delta; stack[top++] = VIDA_DOUBLE(g->as.number); }
                else {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Operand must be a number.");
                    return VIDA_RUNTIME_ERROR;
                }
                break;
            }
            case OP_ADD_GLOBAL: {
                uint16_t slot = READ_SHORT();
                if (VIDA_UNLIKELY(slot >= (uint16_t)nglobals)) {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Undefined global variable.");
                    return VIDA_RUNTIME_ERROR;
                }
                VidaValue rhs = stack[--top];
                VidaValue *g = &globals[slot];
                if (VIDA_LIKELY(g->type == VAL_INT && rhs.type == VAL_INT)) {
                    g->as.integer += rhs.as.integer;
                    stack[top++] = VIDA_INT(g->as.integer);
                } else if (g->type == VAL_STRING && rhs.type == VAL_STRING) {
                    if (VIDA_UNLIKELY(!vida_append_inline(g, &rhs))) {
                        vida_free_inline(&rhs);
                        VIDA_SYNC();
                        runtime_error_at(vm, chunk, ip, "Out of memory.");
                        return VIDA_RUNTIME_ERROR;
                    }
                    vida_free_inline(&rhs);
                    stack[top++] = vida_copy_inline(*g);
                } else if (g->type == VAL_DOUBLE && rhs.type == VAL_DOUBLE) {
                    g->as.number += rhs.as.number;
                    stack[top++] = VIDA_DOUBLE(g->as.number);
                } else if (g->type == VAL_INT && rhs.type == VAL_DOUBLE) {
                    double nv = (double)g->as.integer + rhs.as.number;
                    g->type = VAL_DOUBLE; g->as.number = nv;
                    stack[top++] = VIDA_DOUBLE(nv);
                } else if (g->type == VAL_DOUBLE && rhs.type == VAL_INT) {
                    g->as.number += (double)rhs.as.integer;
                    stack[top++] = VIDA_DOUBLE(g->as.number);
                } else {
                    vida_free_inline(&rhs);
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Operands must be two numbers or two strings.");
                    return VIDA_RUNTIME_ERROR;
                }
                break;
            }
            case OP_ADD_LOCAL: {
                uint8_t slot = READ_BYTE();
                VidaValue rhs = stack[--top];
                VidaValue *g = &slots[slot];
                if (VIDA_LIKELY(g->type == VAL_INT && rhs.type == VAL_INT)) {
                    g->as.integer += rhs.as.integer;
                    stack[top++] = VIDA_INT(g->as.integer);
                } else if (g->type == VAL_STRING && rhs.type == VAL_STRING) {
                    if (VIDA_UNLIKELY(!vida_append_inline(g, &rhs))) {
                        vida_free_inline(&rhs);
                        VIDA_SYNC();
                        runtime_error_at(vm, chunk, ip, "Out of memory.");
                        return VIDA_RUNTIME_ERROR;
                    }
                    vida_free_inline(&rhs);
                    stack[top++] = vida_copy_inline(*g);
                } else if (g->type == VAL_DOUBLE && rhs.type == VAL_DOUBLE) {
                    g->as.number += rhs.as.number;
                    stack[top++] = VIDA_DOUBLE(g->as.number);
                } else if (g->type == VAL_INT && rhs.type == VAL_DOUBLE) {
                    double nv = (double)g->as.integer + rhs.as.number;
                    g->type = VAL_DOUBLE; g->as.number = nv;
                    stack[top++] = VIDA_DOUBLE(nv);
                } else if (g->type == VAL_DOUBLE && rhs.type == VAL_INT) {
                    g->as.number += (double)rhs.as.integer;
                    stack[top++] = VIDA_DOUBLE(g->as.number);
                } else {
                    vida_free_inline(&rhs);
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Operands must be two numbers or two strings.");
                    return VIDA_RUNTIME_ERROR;
                }
                break;
            }
            case OP_INCR_GLOBAL_STMT: {
                uint16_t slot = READ_SHORT();
                int16_t delta = READ_I16();
                if (VIDA_UNLIKELY(slot >= (uint16_t)nglobals)) {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Undefined global variable.");
                    return VIDA_RUNTIME_ERROR;
                }
                VidaValue *g = &globals[slot];
                if (VIDA_LIKELY(g->type == VAL_INT)) g->as.integer += delta;
                else if (g->type == VAL_DOUBLE) g->as.number += (double)delta;
                else {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Operand must be a number.");
                    return VIDA_RUNTIME_ERROR;
                }
                break;
            }
            case OP_INCR_LOCAL_STMT: {
                uint8_t slot = READ_BYTE();
                int16_t delta = READ_I16();
                VidaValue *g = &slots[slot];
                if (VIDA_LIKELY(g->type == VAL_INT)) g->as.integer += delta;
                else if (g->type == VAL_DOUBLE) g->as.number += (double)delta;
                else {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Operand must be a number.");
                    return VIDA_RUNTIME_ERROR;
                }
                break;
            }
            case OP_ADD_GLOBAL_STMT: {
                uint16_t slot = READ_SHORT();
                if (VIDA_UNLIKELY(slot >= (uint16_t)nglobals)) {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Undefined global variable.");
                    return VIDA_RUNTIME_ERROR;
                }
                VidaValue rhs = stack[--top];
                VidaValue *g = &globals[slot];
                if (VIDA_LIKELY(g->type == VAL_INT && rhs.type == VAL_INT)) {
                    g->as.integer += rhs.as.integer;
                } else if (g->type == VAL_STRING && rhs.type == VAL_STRING) {
                    if (VIDA_UNLIKELY(!vida_append_inline(g, &rhs))) {
                        vida_free_inline(&rhs);
                        VIDA_SYNC();
                        runtime_error_at(vm, chunk, ip, "Out of memory.");
                        return VIDA_RUNTIME_ERROR;
                    }
                    vida_free_inline(&rhs);
                } else if (g->type == VAL_DOUBLE && rhs.type == VAL_DOUBLE) {
                    g->as.number += rhs.as.number;
                } else if (g->type == VAL_INT && rhs.type == VAL_DOUBLE) {
                    g->type = VAL_DOUBLE;
                    g->as.number = (double)g->as.integer + rhs.as.number;
                } else if (g->type == VAL_DOUBLE && rhs.type == VAL_INT) {
                    g->as.number += (double)rhs.as.integer;
                } else {
                    vida_free_inline(&rhs);
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Operands must be two numbers or two strings.");
                    return VIDA_RUNTIME_ERROR;
                }
                break;
            }
            case OP_ADD_LOCAL_STMT: {
                uint8_t slot = READ_BYTE();
                VidaValue rhs = stack[--top];
                VidaValue *g = &slots[slot];
                if (VIDA_LIKELY(g->type == VAL_INT && rhs.type == VAL_INT)) {
                    g->as.integer += rhs.as.integer;
                } else if (g->type == VAL_STRING && rhs.type == VAL_STRING) {
                    if (VIDA_UNLIKELY(!vida_append_inline(g, &rhs))) {
                        vida_free_inline(&rhs);
                        VIDA_SYNC();
                        runtime_error_at(vm, chunk, ip, "Out of memory.");
                        return VIDA_RUNTIME_ERROR;
                    }
                    vida_free_inline(&rhs);
                } else if (g->type == VAL_DOUBLE && rhs.type == VAL_DOUBLE) {
                    g->as.number += rhs.as.number;
                } else if (g->type == VAL_INT && rhs.type == VAL_DOUBLE) {
                    g->type = VAL_DOUBLE;
                    g->as.number = (double)g->as.integer + rhs.as.number;
                } else if (g->type == VAL_DOUBLE && rhs.type == VAL_INT) {
                    g->as.number += (double)rhs.as.integer;
                } else {
                    vida_free_inline(&rhs);
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Operands must be two numbers or two strings.");
                    return VIDA_RUNTIME_ERROR;
                }
                break;
            }
            case OP_SUB_GLOBAL: {
                uint16_t slot = READ_SHORT();
                if (VIDA_UNLIKELY(slot >= (uint16_t)nglobals)) {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Undefined global variable.");
                    return VIDA_RUNTIME_ERROR;
                }
                VidaValue rhs = stack[--top];
                VidaValue *g = &globals[slot];
                if (VIDA_LIKELY(g->type == VAL_INT && rhs.type == VAL_INT)) {
                    g->as.integer -= rhs.as.integer;
                    stack[top++] = VIDA_INT(g->as.integer);
                } else if (g->type == VAL_DOUBLE && rhs.type == VAL_DOUBLE) {
                    g->as.number -= rhs.as.number;
                    stack[top++] = VIDA_DOUBLE(g->as.number);
                } else if (g->type == VAL_INT && rhs.type == VAL_DOUBLE) {
                    double nv = (double)g->as.integer - rhs.as.number;
                    g->type = VAL_DOUBLE; g->as.number = nv;
                    stack[top++] = VIDA_DOUBLE(nv);
                } else if (g->type == VAL_DOUBLE && rhs.type == VAL_INT) {
                    g->as.number -= (double)rhs.as.integer;
                    stack[top++] = VIDA_DOUBLE(g->as.number);
                } else {
                    vida_free_inline(&rhs);
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Operands must be numbers.");
                    return VIDA_RUNTIME_ERROR;
                }
                break;
            }
            case OP_SUB_LOCAL: {
                uint8_t slot = READ_BYTE();
                VidaValue rhs = stack[--top];
                VidaValue *g = &slots[slot];
                if (VIDA_LIKELY(g->type == VAL_INT && rhs.type == VAL_INT)) {
                    g->as.integer -= rhs.as.integer;
                    stack[top++] = VIDA_INT(g->as.integer);
                } else if (g->type == VAL_DOUBLE && rhs.type == VAL_DOUBLE) {
                    g->as.number -= rhs.as.number;
                    stack[top++] = VIDA_DOUBLE(g->as.number);
                } else if (g->type == VAL_INT && rhs.type == VAL_DOUBLE) {
                    double nv = (double)g->as.integer - rhs.as.number;
                    g->type = VAL_DOUBLE; g->as.number = nv;
                    stack[top++] = VIDA_DOUBLE(nv);
                } else if (g->type == VAL_DOUBLE && rhs.type == VAL_INT) {
                    g->as.number -= (double)rhs.as.integer;
                    stack[top++] = VIDA_DOUBLE(g->as.number);
                } else {
                    vida_free_inline(&rhs);
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Operands must be numbers.");
                    return VIDA_RUNTIME_ERROR;
                }
                break;
            }
            case OP_SUB_GLOBAL_STMT: {
                uint16_t slot = READ_SHORT();
                if (VIDA_UNLIKELY(slot >= (uint16_t)nglobals)) {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Undefined global variable.");
                    return VIDA_RUNTIME_ERROR;
                }
                VidaValue rhs = stack[--top];
                VidaValue *g = &globals[slot];
                if (VIDA_LIKELY(g->type == VAL_INT && rhs.type == VAL_INT)) {
                    g->as.integer -= rhs.as.integer;
                } else if (g->type == VAL_DOUBLE && rhs.type == VAL_DOUBLE) {
                    g->as.number -= rhs.as.number;
                } else if (g->type == VAL_INT && rhs.type == VAL_DOUBLE) {
                    g->type = VAL_DOUBLE;
                    g->as.number = (double)g->as.integer - rhs.as.number;
                } else if (g->type == VAL_DOUBLE && rhs.type == VAL_INT) {
                    g->as.number -= (double)rhs.as.integer;
                } else {
                    vida_free_inline(&rhs);
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Operands must be numbers.");
                    return VIDA_RUNTIME_ERROR;
                }
                break;
            }
            case OP_SUB_LOCAL_STMT: {
                uint8_t slot = READ_BYTE();
                VidaValue rhs = stack[--top];
                VidaValue *g = &slots[slot];
                if (VIDA_LIKELY(g->type == VAL_INT && rhs.type == VAL_INT)) {
                    g->as.integer -= rhs.as.integer;
                } else if (g->type == VAL_DOUBLE && rhs.type == VAL_DOUBLE) {
                    g->as.number -= rhs.as.number;
                } else if (g->type == VAL_INT && rhs.type == VAL_DOUBLE) {
                    g->type = VAL_DOUBLE;
                    g->as.number = (double)g->as.integer - rhs.as.number;
                } else if (g->type == VAL_DOUBLE && rhs.type == VAL_INT) {
                    g->as.number -= (double)rhs.as.integer;
                } else {
                    vida_free_inline(&rhs);
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Operands must be numbers.");
                    return VIDA_RUNTIME_ERROR;
                }
                break;
            }
            case OP_SET_GLOBAL_MOVE: {
                uint16_t slot = READ_SHORT();
                if (VIDA_UNLIKELY(slot >= VIDA_GLOBALS_MAX)) {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Too many globals.");
                    return VIDA_RUNTIME_ERROR;
                }
                VidaValue val = stack[--top];
                if (slot < nglobals) {
                    vida_free_inline(&globals[slot]);
                    globals[slot] = val;
                } else if (slot == nglobals) {
                    globals[nglobals++] = val;
                } else {
                    vida_free_inline(&val);
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Undefined global variable.");
                    return VIDA_RUNTIME_ERROR;
                }
                break;
            }
            case OP_SET_LOCAL_MOVE: {
                uint8_t slot = READ_BYTE();
                VidaValue val = stack[--top];
                vida_free_inline(&slots[slot]);
                slots[slot] = val;
                break;
            }
            case OP_CALL: {
                uint8_t argc = READ_BYTE();
                if (VIDA_UNLIKELY(top < (int)argc + 1)) {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Stack underflow in call.");
                    return VIDA_RUNTIME_ERROR;
                }
                VidaValue callee = stack[top - argc - 1]; // shallow borrow
                if (callee.type == VAL_NATIVE) {
                    VidaValue result;
                    bool ok = callee.as.native.fn(argc, &stack[top - argc], &result);
                    for (int i = top - argc - 1; i < top; i++) vida_free_inline(&stack[i]);
                    top -= argc + 1;
                    if (VIDA_UNLIKELY(!ok)) {
                        VIDA_SYNC();
                        runtime_error_at(vm, chunk, ip, "Bad arguments to built-in function.");
                        return VIDA_RUNTIME_ERROR;
                    }
                    stack[top++] = result;
                    break;
                }
                if (VIDA_UNLIKELY(callee.type != VAL_FUNC)) {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Can only call functions.");
                    return VIDA_RUNTIME_ERROR;
                }
                VidaFunc *fn = callee.as.func;
                if (VIDA_UNLIKELY(argc != fn->arity)) {
                    char msg[96];
                    snprintf(msg, sizeof(msg), "Expected %d args but got %d.", fn->arity, argc);
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, msg);
                    return VIDA_RUNTIME_ERROR;
                }
                if (VIDA_UNLIKELY(vm->frame_count >= VIDA_FRAMES_MAX)) {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Call stack overflow.");
                    return VIDA_RUNTIME_ERROR;
                }
                if (VIDA_UNLIKELY(top + VIDA_FRAME_STACK >= VIDA_STACK_MAX)) {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Stack overflow.");
                    return VIDA_RUNTIME_ERROR;
                }
                frame->ip = ip;
                vm->stack_top = top;
                frame = &vm->frames[vm->frame_count++];
                frame->func = fn;
                frame->slots = &stack[top - argc - 1];
                frame->ip = fn->chunk.code;
                VIDA_LOAD_FRAME();
                break;
            }
            case OP_RETURN: {
                if (VIDA_UNLIKELY(top <= (int)(frame->slots - stack))) {
                    VIDA_SYNC();
                    runtime_error_at(vm, chunk, ip, "Stack underflow in return.");
                    return VIDA_RUNTIME_ERROR;
                }
                VidaValue res = stack[--top];
                VidaValue *base = frame->slots;
                for (VidaValue *p = base; p < &stack[top]; p++) vida_free_inline(p);
                top = (int)(base - stack);
                if (--vm->frame_count == 0) {
                    vida_free_inline(&res);
                    VIDA_SYNC();
                    return VIDA_OK;
                }
                frame = &vm->frames[vm->frame_count - 1];
                VIDA_LOAD_FRAME();
                stack[top++] = res; // transfer ownership, no copy
                break;
            }
            case OP_HALT: {
                VIDA_SYNC();
                return VIDA_OK;
            }
        }
    }
#endif

#undef VIDA_SYNC
#undef READ_BYTE
#undef READ_SHORT
#undef READ_I16
#undef PUSH
#undef POP
#undef PEEK
#undef DISPATCH
#undef DISPATCH_NEXT
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
