#ifndef VIDA_VM_H
#define VIDA_VM_H

#include "vida_value.h"

typedef enum {
    OP_CONSTANT,    // push constant (u8 index)
    OP_CONSTANT_LONG, // push constant (u16 index)
    OP_NIL,         // push nil
    OP_TRUE,        // push true
    OP_FALSE,       // push false
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_MODULO,
    OP_NEGATE,
    OP_NOT,
    OP_EQUAL,
    OP_NOT_EQUAL,
    OP_LESS,
    OP_LESS_EQUAL,
    OP_GREATER,
    OP_GREATER_EQUAL,
    OP_PRINT,       // print top of stack (pops)
    OP_PRINT_GLOBAL, // print globals[slot] directly, no copy (u16)
    OP_PRINT_LOCAL, // print stack[slot] directly, no copy (u8)
    OP_PRINT_CONST, // print constants[idx] directly, no copy (u8)
    OP_PRINT_CONST_LONG, // print constants[idx] directly, no copy (u16)
    OP_NOP,         // do nothing (peephole padding)
    OP_POP,         // discard top of stack
    OP_GET_GLOBAL,  // get global by slot (u16)
    OP_SET_GLOBAL,  // set global by slot, keep stack value (u16)
    OP_DEFINE_GLOBAL, // define global by slot, pops stack value (u16)
    OP_GET_LOCAL,   // get local variable (u8)
    OP_SET_LOCAL,   // set local variable (u8)
    OP_JUMP,        // unconditional jump (u16)
    OP_JUMP_IF_FALSE, // conditional jump, peek (u16)
    OP_JUMP_IF_FALSE_POP, // conditional jump, pops condition (u16)
    OP_LOOP,        // loop back (u16)
    OP_INCR_GLOBAL, // globals[slot] += delta (u16 slot + i16 delta, pushes new value)
    OP_INCR_LOCAL,  // stack[slot] += delta (u8 slot + i16 delta, pushes new value)
    OP_ADD_GLOBAL,  // globals[slot] += pop() (u16 slot, pushes new value)
    OP_ADD_LOCAL,   // stack[slot] += pop() (u8 slot, pushes new value)
    OP_INCR_GLOBAL_STMT, // same as INCR_GLOBAL but no push (statement context)
    OP_INCR_LOCAL_STMT,  // same as INCR_LOCAL but no push
    OP_ADD_GLOBAL_STMT,   // same as ADD_GLOBAL but no push
    OP_ADD_LOCAL_STMT,   // same as ADD_LOCAL but no push
    OP_SUB_GLOBAL,  // globals[slot] -= pop() (u16 slot, pushes new value)
    OP_SUB_LOCAL,   // stack[slot] -= pop() (u8 slot, pushes new value)
    OP_SUB_GLOBAL_STMT,  // same as SUB_GLOBAL but no push
    OP_SUB_LOCAL_STMT,   // same as SUB_LOCAL but no push
    OP_SET_GLOBAL_MOVE, // globals[slot] = pop() (u16 slot, move, no copy)
    OP_SET_LOCAL_MOVE, // stack[slot] = pop() (u8 slot, move, no copy)
    OP_CALL,        // call callee with argc args (u8); natives + user fns
    OP_RETURN,
    OP_HALT,
} VidaOpCode;

typedef struct {
    uint8_t *code;       // bytecode array
    int count;
    int capacity;
    VidaValue *constants; // constant pool
    int const_count;
    int const_capacity;
    int *lines;          // line numbers for each opcode
} VidaChunk;

void vida_chunk_init(VidaChunk *chunk);
void vida_chunk_write(VidaChunk *chunk, uint8_t byte, int line);
int vida_chunk_add_constant(VidaChunk *chunk, VidaValue value);
void vida_chunk_free(VidaChunk *chunk);

#define VIDA_STACK_MAX 1024
#define VIDA_GLOBALS_MAX 1024
#define VIDA_FRAMES_MAX 64
#define VIDA_FRAME_STACK 256 // max locals/temps reserved per call frame

// A compiled function. Lives in constant pools (one owner ref) and is shared
// by value copies on the stack/globals (one ref each). No closures yet:
// nested `fn` binds a global, so functions only see globals + params.
struct VidaFunc {
    char *name;      // heap copy; may be "script"/"anon"
    int arity;
    VidaChunk chunk; // owned bytecode + constants
    int refcount;
};

VidaFunc *vida_func_new(const char *name, int name_len, int arity);
void vida_func_retain(VidaFunc *func);
void vida_func_release(VidaFunc *func);

typedef struct {
    VidaFunc *func;   // borrowed from stack slot 0 (which owns a ref)
    uint8_t *ip;
    VidaValue *slots; // == stack[base]; slot 0 is reserved (callee/NIL)
} VidaFrame;

typedef struct {
    VidaFrame frames[VIDA_FRAMES_MAX];
    int frame_count;
    VidaValue stack[VIDA_STACK_MAX];
    int stack_top;
    VidaValue globals[VIDA_GLOBALS_MAX];
    int global_count;
    VidaChunk *chunk; // current chunk (error reporting)
    uint8_t *ip;      // current ip (error reporting)
} VidaVM;

typedef enum {
    VIDA_OK,
    VIDA_COMPILE_ERROR,
    VIDA_RUNTIME_ERROR,
} VidaResult;

void vida_vm_init(VidaVM *vm);
void vida_vm_free(VidaVM *vm);
VidaResult vida_vm_run(VidaVM *vm, VidaChunk *chunk);

#endif // VIDA_VM_H
