#ifndef VIDA_COMPILER_H
#define VIDA_COMPILER_H

#include "vida_lexer.h"
#include "vida_vm.h"

typedef struct {
    VidaToken name;
    int depth;       // scope depth (0 = global)
    bool used;       // set on read; unread locals are a compile error
} VidaLocal;

#define VIDA_MAX_LOCALS 256
#define VIDA_MAX_GLOBALS 1024
#define VIDA_MAX_FUNCS 64

// One per function being compiled (root == script). Locals/slots are
// frame-relative; slot 0 is reserved (callee). Globals live on the shared
// VidaCompiler so nested `fn` still binds globally (no closures yet).
typedef struct FuncCompiler {
    struct FuncCompiler *enclosing;
    VidaLocal locals[VIDA_MAX_LOCALS];
    int local_count;
    int scope_depth;
    VidaChunk *chunk;
    VidaFunc *func; // NULL for the script root
    int arity;
} FuncCompiler;

typedef struct {
    const char *start;
    int length;
    int line;        // declaration line, for diagnostics
    bool used;       // set on read; unread globals warn (not error: REPL/def-later)
} VidaGlobalName;

typedef struct {
    VidaLexer lexer;
    VidaToken current;
    VidaToken previous;
    VidaChunk *chunk;     // chunk being compiled into
    bool had_error;
    bool panic_mode;

    VidaLocal locals[VIDA_MAX_LOCALS];
    int local_count;
    int scope_depth;

    VidaGlobalName globals[VIDA_MAX_GLOBALS];
    int global_count;

    FuncCompiler *fn; // current function (root == script)
    FuncCompiler funcs[VIDA_MAX_FUNCS];
    int func_depth;

    bool fused_stmt; // last emit was a no-push STMT op (skip trailing POP)
    bool allow_stmt_fusion; // true only directly inside an expr_statement
    int assign_depth; // nesting level of `=` RHS parsing (STMT only at 0)
} VidaCompiler;

// Main entry point: compile source code into a chunk
bool vida_compile(const char *source, VidaChunk *chunk);

#endif
