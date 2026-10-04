#include "vida_compiler.h"
#include "vida_native.h"
#include "vida_value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,  // =
    PREC_OR,          // or
    PREC_AND,         // and
    PREC_EQUALITY,    // == !=
    PREC_COMPARISON,  // < > <= >=
    PREC_TERM,        // + -
    PREC_FACTOR,      // * / %
    PREC_UNARY,       // not -
    PREC_CALL,        // . ()
    PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(VidaCompiler *compiler, bool can_assign);

typedef struct {
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
} ParseRule;

// Forward declarations
static void expression(VidaCompiler *compiler);
static void statement(VidaCompiler *compiler);
static void declaration(VidaCompiler *compiler);
static ParseRule* get_rule(VidaTokenType type);
static void parse_precedence(VidaCompiler *compiler, Precedence precedence);

static void error_at(VidaCompiler *compiler, VidaToken *token, const char *message) {
    if (compiler->panic_mode) return;
    compiler->panic_mode = true;
    
    fprintf(stderr, "[line %d] Error", token->line);
    if (token->type == TOK_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type == TOK_ERROR) {
        // Nothing.
    } else {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }
    
    fprintf(stderr, ": %s\n", message);
    compiler->had_error = true;
}

static void error(VidaCompiler *compiler, const char *message) {
    error_at(compiler, &compiler->previous, message);
}

static void error_at_current(VidaCompiler *compiler, const char *message) {
    error_at(compiler, &compiler->current, message);
}

static void advance(VidaCompiler *compiler) {
    compiler->previous = compiler->current;
    
    for (;;) {
        compiler->current = vida_lexer_next(&compiler->lexer);
        if (compiler->current.type != TOK_ERROR) break;
        error_at_current(compiler, compiler->current.start);
    }
}

static void consume(VidaCompiler *compiler, VidaTokenType type, const char *message) {
    if (compiler->current.type == type) {
        advance(compiler);
        return;
    }
    error_at_current(compiler, message);
}

static bool check(VidaCompiler *compiler, VidaTokenType type) {
    return compiler->current.type == type;
}

static bool match(VidaCompiler *compiler, VidaTokenType type) {
    if (!check(compiler, type)) return false;
    advance(compiler);
    return true;
}

// Ignore newlines that are not significant
static void skip_newlines(VidaCompiler *compiler) {
    while (match(compiler, TOK_NEWLINE));
}

// Match statement terminator (semicolon or newline)
static void consume_terminator(VidaCompiler *compiler, const char* message) {
    if (match(compiler, TOK_SEMICOLON) || match(compiler, TOK_NEWLINE)) {
        skip_newlines(compiler);
        return;
    }
    if (compiler->current.type == TOK_EOF || compiler->current.type == TOK_RIGHT_BRACE) {
        return;
    }
    error_at_current(compiler, message);
}

static void emit_byte(VidaCompiler *compiler, uint8_t byte) {
    vida_chunk_write(compiler->fn->chunk, byte, compiler->previous.line);
}

static void emit_bytes(VidaCompiler *compiler, uint8_t byte1, uint8_t byte2) {
    emit_byte(compiler, byte1);
    emit_byte(compiler, byte2);
}

static void emit_short(VidaCompiler *compiler, uint16_t value) {
    emit_byte(compiler, (uint8_t)((value >> 8) & 0xff));
    emit_byte(compiler, (uint8_t)(value & 0xff));
}

static void emit_global_op(VidaCompiler *compiler, uint8_t op, int slot) {
    emit_byte(compiler, op);
    emit_short(compiler, (uint16_t)slot);
}

static int emit_jump(VidaCompiler *compiler, uint8_t instruction) {
    emit_byte(compiler, instruction);
    emit_byte(compiler, 0xff);
    emit_byte(compiler, 0xff);
    return compiler->fn->chunk->count - 2;
}

static void patch_jump(VidaCompiler *compiler, int offset) {
    int jump = compiler->fn->chunk->count - offset - 2;
    if (jump > 65535) {
        error(compiler, "Too much code to jump over.");
    }
    compiler->fn->chunk->code[offset] = (jump >> 8) & 0xff;
    compiler->fn->chunk->code[offset + 1] = jump & 0xff;
}

static void emit_loop(VidaCompiler *compiler, int loop_start) {
    emit_byte(compiler, OP_LOOP);
    int offset = compiler->fn->chunk->count - loop_start + 2;
    if (offset > 65535) error(compiler, "Loop body too large.");
    emit_byte(compiler, (offset >> 8) & 0xff);
    emit_byte(compiler, offset & 0xff);
}

static int make_constant(VidaCompiler *compiler, VidaValue value) {
    int constant = vida_chunk_add_constant(compiler->fn->chunk, value);
    if (constant > 65535) {
        error(compiler, "Too many constants in one chunk.");
        return 0;
    }
    return constant;
}

static void emit_constant(VidaCompiler *compiler, VidaValue value) {
    int constant = make_constant(compiler, value);
    if (constant <= 255) {
        emit_bytes(compiler, OP_CONSTANT, (uint8_t)constant);
    } else {
        emit_byte(compiler, OP_CONSTANT_LONG);
        emit_short(compiler, (uint16_t)constant);
    }
}

static void begin_scope(VidaCompiler *compiler) {
    compiler->fn->scope_depth++;
}

static bool is_blank_token(VidaToken *t) {
    return t->length == 1 && t->start[0] == '_';
}

static void end_scope(VidaCompiler *compiler) {
    compiler->fn->scope_depth--;
    while (compiler->fn->local_count > 0 &&
           compiler->fn->locals[compiler->fn->local_count - 1].depth > compiler->fn->scope_depth) {
        VidaLocal *dead = &compiler->fn->locals[compiler->fn->local_count - 1];
        if (!dead->used && !is_blank_token(&dead->name)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "declared '%.*s' and never used (use it or remove it)",
                     dead->name.length, dead->name.start);
            error_at(compiler, &dead->name, msg);
        }
        emit_byte(compiler, OP_POP);
        compiler->fn->local_count--;
    }
}

static bool globals_equal(VidaGlobalName *a, VidaToken *b) {
    if (a->length != b->length) return false;
    return memcmp(a->start, b->start, (size_t)b->length) == 0;
}

static int resolve_global(VidaCompiler *compiler, VidaToken *name) {
    for (int i = compiler->global_count - 1; i >= 0; i--) {
        if (globals_equal(&compiler->globals[i], name)) return i;
    }
    return -1;
}

static int declare_global(VidaCompiler *compiler, VidaToken *name) {
    int existing = resolve_global(compiler, name);
    if (existing != -1) return existing;
    if (compiler->global_count >= VIDA_MAX_GLOBALS) {
        error(compiler, "Too many global variables.");
        return 0;
    }
    compiler->globals[compiler->global_count].start = name->start;
    compiler->globals[compiler->global_count].length = name->length;
    compiler->globals[compiler->global_count].line = name->line;
    compiler->globals[compiler->global_count].used = false;
    return compiler->global_count++;
}

static bool identifiers_equal(VidaToken *a, VidaToken *b) {
    if (a->length != b->length) return false;
    return memcmp(a->start, b->start, a->length) == 0;
}

static int resolve_local(VidaCompiler *compiler, VidaToken *name) {
    for (int i = compiler->fn->local_count - 1; i >= 0; i--) {
        VidaLocal *local = &compiler->fn->locals[i];
        if (identifiers_equal(name, &local->name)) {
            if (local->depth == -1) {
                error(compiler, "Cannot read local variable in its own initializer.");
            }
            return i;
        }
    }
    return -1;
}

static void add_local(VidaCompiler *compiler, VidaToken name) {
    if (compiler->fn->local_count == VIDA_MAX_LOCALS) {
        error(compiler, "Too many local variables in function.");
        return;
    }
    VidaLocal *local = &compiler->fn->locals[compiler->fn->local_count++];
    local->name = name;
    local->depth = -1; // uninitialized
    local->used = false;
}

static void declare_variable(VidaCompiler *compiler) {
    if (compiler->fn->scope_depth == 0) return;
    VidaToken *name = &compiler->previous;
    
    for (int i = compiler->fn->local_count - 1; i >= 0; i--) {
        VidaLocal *local = &compiler->fn->locals[i];
        if (local->depth != -1 && local->depth < compiler->fn->scope_depth) break;
        if (identifiers_equal(name, &local->name)) {
            error(compiler, "Already a variable with this name in this scope.");
        }
    }
    add_local(compiler, *name);
}

static int parse_variable(VidaCompiler *compiler, const char *error_msg) {
    consume(compiler, TOK_IDENTIFIER, error_msg);
    declare_variable(compiler);
    if (compiler->fn->scope_depth > 0) return 0;
    return declare_global(compiler, &compiler->previous);
}

static void mark_initialized(VidaCompiler *compiler) {
    if (compiler->fn->scope_depth == 0) return;
    compiler->fn->locals[compiler->fn->local_count - 1].depth = compiler->fn->scope_depth;
}

static void define_variable(VidaCompiler *compiler, int global) {
    if (compiler->fn->scope_depth > 0) {
        mark_initialized(compiler);
        return;
    }
    emit_global_op(compiler, OP_DEFINE_GLOBAL, global);
}

// --- Functions ---

static void begin_function(VidaCompiler *compiler, const char *name, int name_len) {
    if (compiler->func_depth >= VIDA_MAX_FUNCS) {
        error(compiler, "Too many nested functions.");
        return;
    }
    FuncCompiler *fn = &compiler->funcs[compiler->func_depth++];
    fn->enclosing = compiler->fn;
    fn->local_count = 0;
    fn->scope_depth = 1; // params/locals live here; 0 means global
    fn->arity = 0;
    fn->func = vida_func_new(name, name_len, 0);
    if (!fn->func) {
        error(compiler, "Out of memory.");
        compiler->func_depth--;
        return;
    }
    fn->chunk = &fn->func->chunk;
    compiler->fn = fn;
    // Reserve slot 0 (callee at runtime). Never matches a real name.
    VidaToken blank = {TOK_IDENTIFIER, "", 0, 0, 0};
    add_local(compiler, blank);
    compiler->fn->locals[0].depth = 0;
    compiler->fn->locals[0].used = true;
}

static VidaFunc *end_function(VidaCompiler *compiler) {
    FuncCompiler *fn = compiler->fn;
    // Unused params/body-top locals (body blocks already end_scoped themselves).
    for (int i = 1; i < fn->local_count; i++) {
        VidaLocal *local = &fn->locals[i];
        if (local->depth >= 1 && !local->used && !is_blank_token(&local->name)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "declared '%.*s' and never used (use it or remove it)",
                     local->name.length, local->name.start);
            error_at(compiler, &local->name, msg);
        }
    }
    emit_byte(compiler, OP_NIL); // implicit `return nil`
    emit_byte(compiler, OP_RETURN);
    fn->func->arity = fn->arity;
    compiler->fn = fn->enclosing;
    compiler->func_depth--;
    return fn->func;
}

// Compile `name?(params) { body }` after `fn` (and optional name) was consumed.
// Leaves the function value on the stack.
static void fn_body(VidaCompiler *compiler, const char *name, int name_len) {
    begin_function(compiler, name, name_len);
    consume(compiler, TOK_LEFT_PAREN, "Expect '(' after function name.");
    if (!check(compiler, TOK_RIGHT_PAREN)) {
        do {
            if (compiler->fn->arity >= 255) {
                error(compiler, "Too many parameters (max 255).");
                return;
            }
            consume(compiler, TOK_IDENTIFIER, "Expect parameter name.");
            declare_variable(compiler);
            mark_initialized(compiler);
            compiler->fn->arity++;
        } while (match(compiler, TOK_COMMA));
    }
    consume(compiler, TOK_RIGHT_PAREN, "Expect ')' after parameters.");
    consume(compiler, TOK_LEFT_BRACE, "Expect '{' before function body.");
    begin_scope(compiler);
    while (!check(compiler, TOK_RIGHT_BRACE) && !check(compiler, TOK_EOF)) {
        skip_newlines(compiler);
        if (check(compiler, TOK_RIGHT_BRACE)) break;
        declaration(compiler);
    }
    consume(compiler, TOK_RIGHT_BRACE, "Expect '}' after function body.");
    end_scope(compiler);
    VidaFunc *func = end_function(compiler);
    VidaValue v;
    v.type = VAL_FUNC;
    v.as.func = func; // pool owns the creation ref; copies retain
    emit_constant(compiler, v);
}

static void fn_declaration(VidaCompiler *compiler) {
    // `fn` already matched. `fn name(params) { body }` binds a global.
    consume(compiler, TOK_IDENTIFIER, "Expect function name.");
    VidaToken name = compiler->previous;
    int slot = declare_global(compiler, &name);
    fn_body(compiler, name.start, name.length);
    emit_global_op(compiler, OP_DEFINE_GLOBAL, slot);
}

static void fn_expression(VidaCompiler *compiler, bool can_assign) {
    (void)can_assign;
    // Anonymous (or optionally named, for better `<fn name>` prints).
    const char *name = "anon";
    int name_len = 4;
    if (compiler->current.type == TOK_IDENTIFIER) {
        advance(compiler);
        name = compiler->previous.start;
        name_len = compiler->previous.length;
    }
    fn_body(compiler, name, name_len);
}

static void return_statement(VidaCompiler *compiler) {
    if (compiler->fn->func == NULL) {
        error(compiler, "Cannot return from top-level code.");
        return;
    }
    if (check(compiler, TOK_SEMICOLON) || check(compiler, TOK_NEWLINE) ||
        check(compiler, TOK_RIGHT_BRACE) || check(compiler, TOK_EOF)) {
        emit_byte(compiler, OP_NIL);
    } else {
        expression(compiler);
    }
    consume_terminator(compiler, "Expect ';' or newline after return value.");
    emit_byte(compiler, OP_RETURN);
}

// --- Parsing rules ---

static void number_int(VidaCompiler *compiler, bool can_assign) {
    (void)can_assign;
    int64_t value = strtoll(compiler->previous.start, NULL, 10);
    emit_constant(compiler, VIDA_INT(value));
}

static void number_float(VidaCompiler *compiler, bool can_assign) {
    (void)can_assign;
    double value = strtod(compiler->previous.start, NULL);
    emit_constant(compiler, VIDA_DOUBLE(value));
}

static void string(VidaCompiler *compiler, bool can_assign) {
    (void)can_assign;
    emit_constant(compiler, vida_value_string(
        compiler->previous.start + 1,
        compiler->previous.length - 2));
}

static void literal(VidaCompiler *compiler, bool can_assign) {
    (void)can_assign;
    switch (compiler->previous.type) {
        case TOK_FALSE: emit_byte(compiler, OP_FALSE); break;
        case TOK_TRUE: emit_byte(compiler, OP_TRUE); break;
        case TOK_NIL: emit_byte(compiler, OP_NIL); break;
        default: return; // Unreachable.
    }
}

static void grouping(VidaCompiler *compiler, bool can_assign) {
    (void)can_assign;
    expression(compiler);
    consume(compiler, TOK_RIGHT_PAREN, "Expect ')' after expression.");
}

static void call_(VidaCompiler *compiler, bool can_assign) {
    (void)can_assign;
    // Callee is already on the stack; compile args left to right.
    int argc = 0;
    if (!check(compiler, TOK_RIGHT_PAREN)) {
        do {
            if (argc >= 255) {
                error(compiler, "Too many call arguments (max 255).");
                return;
            }
            expression(compiler);
            argc++;
        } while (match(compiler, TOK_COMMA));
    }
    consume(compiler, TOK_RIGHT_PAREN, "Expect ')' after call arguments.");
    emit_bytes(compiler, OP_CALL, (uint8_t)argc);
}

static void unary(VidaCompiler *compiler, bool can_assign) {
    (void)can_assign;
    VidaTokenType operator_type = compiler->previous.type;
    parse_precedence(compiler, PREC_UNARY);
    switch (operator_type) {
        case TOK_MINUS: emit_byte(compiler, OP_NEGATE); break;
        case TOK_NOT:   emit_byte(compiler, OP_NOT); break;
        default: return; // Unreachable.
    }
}

static int vida_op_size(uint8_t op) {
    switch (op) {
        case OP_CONSTANT: return 2;
        case OP_CONSTANT_LONG: return 3;
        case OP_GET_GLOBAL:
        case OP_SET_GLOBAL:
        case OP_DEFINE_GLOBAL:
        case OP_ADD_GLOBAL:
        case OP_ADD_GLOBAL_STMT:
        case OP_SUB_GLOBAL:
        case OP_SUB_GLOBAL_STMT:
        case OP_SET_GLOBAL_MOVE:
        case OP_PRINT_GLOBAL:
        case OP_PRINT_CONST_LONG:
        case OP_JUMP:
        case OP_JUMP_IF_FALSE:
        case OP_JUMP_IF_FALSE_POP:
        case OP_LOOP:
            return 3;
        case OP_GET_LOCAL:
        case OP_SET_LOCAL:
        case OP_SET_LOCAL_MOVE:
        case OP_ADD_LOCAL:
        case OP_ADD_LOCAL_STMT:
        case OP_SUB_LOCAL:
        case OP_SUB_LOCAL_STMT:
        case OP_PRINT_LOCAL:
        case OP_PRINT_CONST:
        case OP_CALL:
            return 2;
        case OP_INCR_GLOBAL:
        case OP_INCR_GLOBAL_STMT:
            return 5;
        case OP_INCR_LOCAL:
        case OP_INCR_LOCAL_STMT:
            return 4;
        default: return 1; // single-byte ops (NIL/TRUE/FALSE/ADD/.../PRINT/POP/RETURN/HALT)
    }
}

// Returns true if the chunk ends with exactly two adjacent u8 OP_CONSTANTs.
static bool chunk_ends_with_two_consts(VidaChunk *chunk, int *ai, int *bi) {
    int count = chunk->count;
    if (count < 4) return false;
    // Walk from the start so multi-byte ops can't desync the check.
    int prev_start = -1, last_start = -1;
    int pos = 0;
    while (pos < count) {
        uint8_t op = chunk->code[pos];
        int sz = vida_op_size(op);
        if (sz <= 0 || pos + sz > count) return false;
        prev_start = last_start;
        last_start = pos;
        pos += sz;
    }
    if (pos != count || prev_start < 0 || last_start < 0) return false;
    if (chunk->code[prev_start] != OP_CONSTANT) return false;
    if (chunk->code[last_start] != OP_CONSTANT) return false;
    if (last_start != prev_start + 2 || count != last_start + 2) return false;
    *ai = chunk->code[prev_start + 1];
    *bi = chunk->code[last_start + 1];
    return true;
}

static bool try_fold_constants(VidaCompiler *compiler, VidaTokenType op) {
    // Fold `CONST a, CONST b, <op>` emitted back-to-back into a single CONST.
    VidaChunk *chunk = compiler->fn->chunk;
    int ai = 0, bi = 0;
    if (!chunk_ends_with_two_consts(chunk, &ai, &bi)) return false;
    if (ai >= chunk->const_count || bi >= chunk->const_count) return false;
    VidaValue a = chunk->constants[ai];
    VidaValue b = chunk->constants[bi];
    VidaValue out;
    bool folded = false;
    if (a.type == VAL_INT && b.type == VAL_INT) {
        int64_t x = a.as.integer, y = b.as.integer;
        switch (op) {
            case TOK_PLUS: out = VIDA_INT(x + y); folded = true; break;
            case TOK_MINUS: out = VIDA_INT(x - y); folded = true; break;
            case TOK_STAR: out = VIDA_INT(x * y); folded = true; break;
            case TOK_SLASH: if (y != 0) { out = VIDA_INT(x / y); folded = true; } break;
            case TOK_PERCENT: if (y != 0) { out = VIDA_INT(x % y); folded = true; } break;
            case TOK_EQUAL_EQUAL: out = VIDA_BOOL(x == y); folded = true; break;
            case TOK_BANG_EQUAL: out = VIDA_BOOL(x != y); folded = true; break;
            case TOK_LESS: out = VIDA_BOOL(x < y); folded = true; break;
            case TOK_LESS_EQUAL: out = VIDA_BOOL(x <= y); folded = true; break;
            case TOK_GREATER: out = VIDA_BOOL(x > y); folded = true; break;
            case TOK_GREATER_EQUAL: out = VIDA_BOOL(x >= y); folded = true; break;
            default: break;
        }
    } else if ((a.type == VAL_DOUBLE || a.type == VAL_INT) &&
               (b.type == VAL_DOUBLE || b.type == VAL_INT) &&
               (op == TOK_PLUS || op == TOK_MINUS || op == TOK_STAR ||
                op == TOK_SLASH || op == TOK_EQUAL_EQUAL || op == TOK_BANG_EQUAL ||
                op == TOK_LESS || op == TOK_LESS_EQUAL ||
                op == TOK_GREATER || op == TOK_GREATER_EQUAL)) {
        double x = (a.type == VAL_INT) ? (double)a.as.integer : a.as.number;
        double y = (b.type == VAL_INT) ? (double)b.as.integer : b.as.number;
        switch (op) {
            case TOK_PLUS: out = VIDA_DOUBLE(x + y); folded = true; break;
            case TOK_MINUS: out = VIDA_DOUBLE(x - y); folded = true; break;
            case TOK_STAR: out = VIDA_DOUBLE(x * y); folded = true; break;
            case TOK_SLASH: out = VIDA_DOUBLE(x / y); folded = true; break;
            case TOK_EQUAL_EQUAL: out = VIDA_BOOL(x == y); folded = true; break;
            case TOK_BANG_EQUAL: out = VIDA_BOOL(x != y); folded = true; break;
            case TOK_LESS: out = VIDA_BOOL(x < y); folded = true; break;
            case TOK_LESS_EQUAL: out = VIDA_BOOL(x <= y); folded = true; break;
            case TOK_GREATER: out = VIDA_BOOL(x > y); folded = true; break;
            case TOK_GREATER_EQUAL: out = VIDA_BOOL(x >= y); folded = true; break;
            default: break;
        }
    } else if (a.type == VAL_STRING && b.type == VAL_STRING && op == TOK_PLUS) {
        int length = a.as.string.length + b.as.string.length;
        char *chars = (char *)malloc((size_t)length + 1);
        if (chars) {
            memcpy(chars, a.as.string.chars, (size_t)a.as.string.length);
            memcpy(chars + a.as.string.length, b.as.string.chars, (size_t)b.as.string.length);
            chars[length] = '\0';
            out = vida_value_take_string(chars, length);
            folded = true;
        }
    }
    if (!folded) return false;
    chunk->count -= 4; // drop CONST a, CONST b (dead pool entries stay, harmless)
    emit_constant(compiler, out);
    return true;
}

static void binary(VidaCompiler *compiler, bool can_assign) {
    (void)can_assign;
    VidaTokenType operator_type = compiler->previous.type;
    ParseRule *rule = get_rule(operator_type);
    parse_precedence(compiler, (Precedence)(rule->precedence + 1));

    // Single-op cases first try compile-time folding (same semantics, zero runtime cost).
    switch (operator_type) {
        case TOK_PLUS:
            if (try_fold_constants(compiler, operator_type)) return;
            emit_byte(compiler, OP_ADD); break;
        case TOK_MINUS:
            if (try_fold_constants(compiler, operator_type)) return;
            emit_byte(compiler, OP_SUBTRACT); break;
        case TOK_STAR:
            if (try_fold_constants(compiler, operator_type)) return;
            emit_byte(compiler, OP_MULTIPLY); break;
        case TOK_SLASH:
            if (try_fold_constants(compiler, operator_type)) return;
            emit_byte(compiler, OP_DIVIDE); break;
        case TOK_PERCENT:
            if (try_fold_constants(compiler, operator_type)) return;
            emit_byte(compiler, OP_MODULO); break;
        case TOK_EQUAL_EQUAL:
            if (try_fold_constants(compiler, operator_type)) return;
            emit_byte(compiler, OP_EQUAL); break;
        case TOK_GREATER:
            if (try_fold_constants(compiler, operator_type)) return;
            emit_byte(compiler, OP_GREATER); break;
        case TOK_LESS:
            if (try_fold_constants(compiler, operator_type)) return;
            emit_byte(compiler, OP_LESS); break;
        case TOK_LESS_EQUAL:
            if (try_fold_constants(compiler, operator_type)) return;
            emit_byte(compiler, OP_LESS_EQUAL); break;
        case TOK_BANG_EQUAL:
            if (try_fold_constants(compiler, operator_type)) return;
            emit_byte(compiler, OP_NOT_EQUAL); break;
        case TOK_GREATER_EQUAL:
            if (try_fold_constants(compiler, operator_type)) return;
            emit_byte(compiler, OP_GREATER_EQUAL); break;
        default: return; // Unreachable.
    }
}

static void mark_used(VidaCompiler *compiler, bool is_local, int slot) {
    if (is_local) {
        compiler->fn->locals[slot].used = true;
    } else if (slot >= 0 && slot < compiler->global_count) {
        compiler->globals[slot].used = true;
    }
}

static void emit_get_for_token(VidaCompiler *compiler, VidaToken *t) {
    int local = resolve_local(compiler, t);
    if (local != -1) {
        compiler->fn->locals[local].used = true;
        emit_bytes(compiler, OP_GET_LOCAL, (uint8_t)local);
        return;
    }
    int g = resolve_global(compiler, t);
    if (g == -1) g = declare_global(compiler, t);
    compiler->globals[g].used = true;
    emit_global_op(compiler, OP_GET_GLOBAL, g);
}

static bool token_is_terminator(VidaTokenType t) {
    return t == TOK_SEMICOLON || t == TOK_NEWLINE || t == TOK_RIGHT_BRACE ||
           t == TOK_RIGHT_PAREN || t == TOK_EOF;
}

static bool token_is_stmt_end(VidaTokenType t) {
    return t == TOK_SEMICOLON || t == TOK_NEWLINE || t == TOK_RIGHT_BRACE ||
           t == TOK_EOF;
}

static void named_variable(VidaCompiler *compiler, VidaToken name, bool can_assign) {
    uint8_t get_op, set_op;
    int arg = resolve_local(compiler, &name);
    bool is_local = (arg != -1);
    if (is_local) {
        get_op = OP_GET_LOCAL;
        set_op = OP_SET_LOCAL;
    } else {
        // Slot-indexed global: reuse slot if known, else allocate.
        // Runtime still reports "Undefined global" on loads of never-defined slots.
        arg = resolve_global(compiler, &name);
        if (arg == -1) arg = declare_global(compiler, &name);
        get_op = OP_GET_GLOBAL;
        set_op = OP_SET_GLOBAL;
    }

    if (can_assign && match(compiler, TOK_EQUAL)) {
        // Fast paths for `x = x + K` (INCR) and `x = x +/- y` (ADD/SUB_GLOBAL).
        // Only when RHS is exactly `x +/- <single>` followed by a terminator,
        // so we never break larger expressions like `x = x + y + z`.
        if (compiler->current.type == TOK_IDENTIFIER &&
            identifiers_equal(&compiler->current, &name)) {
            VidaLexer tmp = compiler->lexer;
            VidaToken op = vida_lexer_next(&tmp);
            VidaToken rhs = vida_lexer_next(&tmp);
            VidaToken after = vida_lexer_next(&tmp);
            bool rhs_is_int = (rhs.type == TOK_INTEGER);
            int64_t kval = 0;
            if (rhs_is_int) kval = strtoll(rhs.start, NULL, 10);
            if ((op.type == TOK_PLUS || op.type == TOK_MINUS) && rhs_is_int &&
                token_is_terminator(after.type)) {
                if (op.type == TOK_MINUS) kval = -kval;
                if (kval >= -32768 && kval <= 32767) {
                    advance(compiler); // second x
                    advance(compiler); // +/-
                    advance(compiler); // K
                    int16_t delta = (int16_t)kval;
                    bool stmt = compiler->allow_stmt_fusion && compiler->assign_depth == 0 &&
                                token_is_stmt_end(after.type);
                    if (is_local) {
                        emit_byte(compiler, stmt ? OP_INCR_LOCAL_STMT : OP_INCR_LOCAL);
                        emit_byte(compiler, (uint8_t)arg);
                        emit_short(compiler, (uint16_t)delta);
                    } else {
                        emit_byte(compiler, stmt ? OP_INCR_GLOBAL_STMT : OP_INCR_GLOBAL);
                        emit_short(compiler, (uint16_t)arg);
                        emit_short(compiler, (uint16_t)delta);
                    }
                    if (stmt) compiler->fused_stmt = true;
                    mark_used(compiler, is_local, arg); // INCR reads x
                    return;
                }
            }
            if ((op.type == TOK_PLUS || op.type == TOK_MINUS) && token_is_terminator(after.type) &&
                (rhs.type == TOK_IDENTIFIER || rhs.type == TOK_INTEGER ||
                 rhs.type == TOK_FLOAT || rhs.type == TOK_STRING)) {
                // `x = x - <string>` is a runtime type error either way; keep
                // strings on the generic path so the error comes from OP_SUBTRACT.
                bool is_sub = (op.type == TOK_MINUS);
                if (is_sub && rhs.type == TOK_STRING) {
                    goto generic_assign;
                }
                advance(compiler); // second x
                advance(compiler); // +/-
                // Emit RHS value (leaves it on stack), then in-place add/sub.
                if (rhs.type == TOK_IDENTIFIER) {
                    VidaToken y = compiler->current;
                    advance(compiler);
                    emit_get_for_token(compiler, &y);
                } else if (rhs.type == TOK_INTEGER) {
                    int64_t v = strtoll(compiler->current.start, NULL, 10);
                    advance(compiler);
                    emit_constant(compiler, VIDA_INT(v));
                } else if (rhs.type == TOK_FLOAT) {
                    double v = strtod(compiler->current.start, NULL);
                    advance(compiler);
                    emit_constant(compiler, VIDA_DOUBLE(v));
                } else {
                    advance(compiler); // string token already in previous
                    emit_constant(compiler, vida_value_string(
                        compiler->previous.start + 1,
                        compiler->previous.length - 2));
                }
                bool stmt = compiler->allow_stmt_fusion && compiler->assign_depth == 0 &&
                              token_is_stmt_end(after.type);
                if (is_local) {
                    uint8_t subop = stmt ? (is_sub ? OP_SUB_LOCAL_STMT : OP_ADD_LOCAL_STMT)
                                         : (is_sub ? OP_SUB_LOCAL : OP_ADD_LOCAL);
                    emit_bytes(compiler, subop, (uint8_t)arg);
                } else {
                    uint8_t subop = stmt ? (is_sub ? OP_SUB_GLOBAL_STMT : OP_ADD_GLOBAL_STMT)
                                         : (is_sub ? OP_SUB_GLOBAL : OP_ADD_GLOBAL);
                    emit_global_op(compiler, subop, arg);
                }
                if (stmt) compiler->fused_stmt = true;
                mark_used(compiler, is_local, arg); // ADD/SUB reads x
                return;
            }
        }
generic_assign:
        compiler->assign_depth++;
        expression(compiler);
        compiler->assign_depth--;
        // In statement context the assigned value is immediately discarded:
        // move it into the slot instead of copy + POP (saves a malloc for strings).
        bool move = compiler->allow_stmt_fusion && compiler->assign_depth == 0 &&
                    token_is_stmt_end(compiler->current.type);
        if (is_local) {
            if (move) {
                emit_bytes(compiler, OP_SET_LOCAL_MOVE, (uint8_t)arg);
                compiler->fused_stmt = true;
            } else {
                emit_bytes(compiler, set_op, (uint8_t)arg);
            }
        } else {
            if (move) {
                emit_global_op(compiler, OP_SET_GLOBAL_MOVE, arg);
                compiler->fused_stmt = true;
            } else {
                emit_global_op(compiler, set_op, arg);
            }
        }
    } else {
        mark_used(compiler, is_local, arg); // plain load
        if (is_local) {
            emit_bytes(compiler, get_op, (uint8_t)arg);
        } else {
            emit_global_op(compiler, get_op, arg);
        }
    }
}

static void variable(VidaCompiler *compiler, bool can_assign) {
    named_variable(compiler, compiler->previous, can_assign);
}

static void and_(VidaCompiler *compiler, bool can_assign) {
    (void)can_assign;
    int end_jump = emit_jump(compiler, OP_JUMP_IF_FALSE);
    emit_byte(compiler, OP_POP);
    parse_precedence(compiler, PREC_AND);
    patch_jump(compiler, end_jump);
}

static void or_(VidaCompiler *compiler, bool can_assign) {
    (void)can_assign;
    int else_jump = emit_jump(compiler, OP_JUMP_IF_FALSE);
    int end_jump = emit_jump(compiler, OP_JUMP);
    
    patch_jump(compiler, else_jump);
    emit_byte(compiler, OP_POP);
    
    parse_precedence(compiler, PREC_OR);
    patch_jump(compiler, end_jump);
}

// Parse rules table — indexed by VidaTokenType enum values
ParseRule rules[] = {
    [TOK_LEFT_PAREN]    = {grouping, call_,   PREC_CALL},
    [TOK_RIGHT_PAREN]   = {NULL,     NULL,   PREC_NONE},
    [TOK_LEFT_BRACE]    = {NULL,     NULL,   PREC_NONE},
    [TOK_RIGHT_BRACE]   = {NULL,     NULL,   PREC_NONE},
    [TOK_LEFT_BRACKET]  = {NULL,     NULL,   PREC_NONE},
    [TOK_RIGHT_BRACKET] = {NULL,     NULL,   PREC_NONE},
    [TOK_COMMA]         = {NULL,     NULL,   PREC_NONE},
    [TOK_SEMICOLON]     = {NULL,     NULL,   PREC_NONE},
    [TOK_COLON]         = {NULL,     NULL,   PREC_NONE},
    [TOK_DOT]           = {NULL,     NULL,   PREC_NONE},
    [TOK_PLUS]          = {NULL,     binary, PREC_TERM},
    [TOK_PLUS_EQUAL]    = {NULL,     NULL,   PREC_NONE},
    [TOK_MINUS]         = {unary,    binary, PREC_TERM},
    [TOK_MINUS_EQUAL]   = {NULL,     NULL,   PREC_NONE},
    [TOK_STAR]          = {NULL,     binary, PREC_FACTOR},
    [TOK_STAR_EQUAL]    = {NULL,     NULL,   PREC_NONE},
    [TOK_SLASH]         = {NULL,     binary, PREC_FACTOR},
    [TOK_SLASH_EQUAL]   = {NULL,     NULL,   PREC_NONE},
    [TOK_PERCENT]       = {NULL,     binary, PREC_FACTOR},
    [TOK_EQUAL]         = {NULL,     NULL,   PREC_NONE},
    [TOK_EQUAL_EQUAL]   = {NULL,     binary, PREC_EQUALITY},
    [TOK_BANG]          = {NULL,     NULL,   PREC_NONE},
    [TOK_BANG_EQUAL]    = {NULL,     binary, PREC_EQUALITY},
    [TOK_LESS]          = {NULL,     binary, PREC_COMPARISON},
    [TOK_LESS_EQUAL]    = {NULL,     binary, PREC_COMPARISON},
    [TOK_GREATER]       = {NULL,     binary, PREC_COMPARISON},
    [TOK_GREATER_EQUAL] = {NULL,     binary, PREC_COMPARISON},
    [TOK_IDENTIFIER]    = {variable, NULL,   PREC_NONE},
    [TOK_STRING]        = {string,   NULL,   PREC_NONE},
    [TOK_INTEGER]       = {number_int,   NULL,   PREC_NONE},
    [TOK_FLOAT]         = {number_float,  NULL,   PREC_NONE},
    [TOK_LET]           = {NULL,     NULL,   PREC_NONE},
    [TOK_FN]            = {fn_expression, NULL,   PREC_NONE},
    [TOK_IF]            = {NULL,     NULL,   PREC_NONE},
    [TOK_ELSE]          = {NULL,     NULL,   PREC_NONE},
    [TOK_WHILE]         = {NULL,     NULL,   PREC_NONE},
    [TOK_FOR]           = {NULL,     NULL,   PREC_NONE},
    [TOK_RETURN]        = {NULL,     NULL,   PREC_NONE},
    [TOK_TRUE]          = {literal,  NULL,   PREC_NONE},
    [TOK_FALSE]         = {literal,  NULL,   PREC_NONE},
    [TOK_NIL]           = {literal,  NULL,   PREC_NONE},
    [TOK_PRINT]         = {NULL,     NULL,   PREC_NONE},
    [TOK_AND]           = {NULL,     and_,   PREC_AND},
    [TOK_OR]            = {NULL,     or_,    PREC_OR},
    [TOK_NOT]           = {unary,    NULL,   PREC_NONE},
    [TOK_NEWLINE]       = {NULL,     NULL,   PREC_NONE},
    [TOK_ERROR]         = {NULL,     NULL,   PREC_NONE},
    [TOK_EOF]           = {NULL,     NULL,   PREC_NONE},
};

static ParseRule* get_rule(VidaTokenType type) {
    return &rules[type];
}

static void parse_precedence(VidaCompiler *compiler, Precedence precedence) {
    advance(compiler);
    ParseFn prefix_rule = get_rule(compiler->previous.type)->prefix;
    if (prefix_rule == NULL) {
        error(compiler, "Expect expression.");
        return;
    }
    
    bool can_assign = precedence <= PREC_ASSIGNMENT;
    prefix_rule(compiler, can_assign);
    
    while (precedence <= get_rule(compiler->current.type)->precedence) {
        advance(compiler);
        ParseFn infix_rule = get_rule(compiler->previous.type)->infix;
        infix_rule(compiler, can_assign);
    }
    
    if (can_assign && match(compiler, TOK_EQUAL)) {
        error(compiler, "Invalid assignment target.");
    }
}

static void expression(VidaCompiler *compiler) {
    parse_precedence(compiler, PREC_ASSIGNMENT);
}

// --- Statements ---

static void block(VidaCompiler *compiler) {
    while (!check(compiler, TOK_RIGHT_BRACE) && !check(compiler, TOK_EOF)) {
        skip_newlines(compiler);
        if (check(compiler, TOK_RIGHT_BRACE)) break;
        declaration(compiler);
    }
    consume(compiler, TOK_RIGHT_BRACE, "Expect '}' after block.");
}

static void let_declaration(VidaCompiler *compiler) {
    int global = parse_variable(compiler, "Expect variable name.");
    
    if (match(compiler, TOK_EQUAL)) {
        expression(compiler);
    } else {
        emit_byte(compiler, OP_NIL);
    }
    
    consume_terminator(compiler, "Expect ';' or newline after variable declaration.");
    define_variable(compiler, global);
}

static bool try_drop_dead_load(VidaCompiler *compiler) {
    // A bare `x;` / `"lit";` / `42;` statement loads a value only to POP it.
    // Drop the load entirely (tail position, nothing follows, so no jump fixup).
    VidaChunk *chunk = compiler->fn->chunk;
    int n = chunk->count;
    if (n < 1) return false;
    int prev_start = -1, last_start = -1;
    int pos = 0;
    while (pos < n) {
        int sz = vida_op_size(chunk->code[pos]);
        if (sz <= 0 || pos + sz > n) return false;
        prev_start = last_start;
        last_start = pos;
        pos += sz;
    }
    if (pos != n || last_start < 0) return false;
    uint8_t op = chunk->code[last_start];
    int sz = 0;
    switch (op) {
        case OP_GET_GLOBAL: sz = 3; break;
        case OP_GET_LOCAL:
        case OP_CONSTANT: sz = 2; break;
        case OP_CONSTANT_LONG: sz = 3; break;
        case OP_NIL:
        case OP_TRUE:
        case OP_FALSE: sz = 1; break;
        default: return false;
    }
    if (last_start + sz != n) return false;
    (void)prev_start;
    chunk->count = last_start; // truncate (lines beyond count are ignored)
    return true;
}

static void expr_statement(VidaCompiler *compiler) {
    compiler->fused_stmt = false;
    compiler->allow_stmt_fusion = true;
    expression(compiler);
    compiler->allow_stmt_fusion = false;
    consume_terminator(compiler, "Expect ';' or newline after expression.");
    if (compiler->fused_stmt) {
        compiler->fused_stmt = false;
        return; // STMT/MOVE op already leaves nothing on the stack
    }
    if (try_drop_dead_load(compiler)) return; // bare load; nothing to pop
    emit_byte(compiler, OP_POP);
    compiler->fused_stmt = false;
}

static void fuse_print_tail(VidaCompiler *compiler) {
    // Rewrite `GET/CONST, PRINT` into direct `PRINT_*` + NOP padding.
    // Same total size, so jump offsets stay valid; the GET/PRINT pair is
    // always emitted consecutively here with no jump landing between them.
    VidaChunk *chunk = compiler->fn->chunk;
    int n = chunk->count;
    if (n < 2 || chunk->code[n-1] != OP_PRINT) return;
    // Decode from the start to find the last two instruction boundaries.
    int prev_start = -1, last_start = -1;
    int pos = 0;
    while (pos < n) {
        int sz = vida_op_size(chunk->code[pos]);
        if (sz <= 0 || pos + sz > n) return; // misaligned; bail safely
        prev_start = last_start;
        last_start = pos;
        pos += sz;
    }
    if (pos != n || last_start != n - 1 || prev_start < 0) return;
    uint8_t first = chunk->code[prev_start];
    if (first == OP_GET_GLOBAL && n - prev_start == 4) {
        chunk->code[prev_start] = OP_PRINT_GLOBAL;
        chunk->code[n-1] = OP_NOP;
    } else if (first == OP_GET_LOCAL && n - prev_start == 3) {
        chunk->code[prev_start] = OP_PRINT_LOCAL;
        chunk->code[n-1] = OP_NOP;
    } else if (first == OP_CONSTANT && n - prev_start == 3) {
        chunk->code[prev_start] = OP_PRINT_CONST;
        chunk->code[n-1] = OP_NOP;
    } else if (first == OP_CONSTANT_LONG && n - prev_start == 4) {
        chunk->code[prev_start] = OP_PRINT_CONST_LONG;
        chunk->code[n-1] = OP_NOP;
    }
}

static void print_statement(VidaCompiler *compiler) {
    consume(compiler, TOK_LEFT_PAREN, "Expect '(' after 'print'.");
    expression(compiler);
    consume(compiler, TOK_RIGHT_PAREN, "Expect ')' after expression.");
    consume_terminator(compiler, "Expect ';' or newline after value.");
    emit_byte(compiler, OP_PRINT);
    fuse_print_tail(compiler);
}

static void if_statement(VidaCompiler *compiler) {
    expression(compiler);

    int then_jump = emit_jump(compiler, OP_JUMP_IF_FALSE_POP);

    skip_newlines(compiler);
    consume(compiler, TOK_LEFT_BRACE, "Expect '{' before if body.");
    begin_scope(compiler);
    block(compiler);
    end_scope(compiler);

    int else_jump = emit_jump(compiler, OP_JUMP);
    patch_jump(compiler, then_jump);

    skip_newlines(compiler);
    if (match(compiler, TOK_ELSE)) {
        skip_newlines(compiler);
        consume(compiler, TOK_LEFT_BRACE, "Expect '{' before else body.");
        begin_scope(compiler);
        block(compiler);
        end_scope(compiler);
    }
    patch_jump(compiler, else_jump);
}

static void while_statement(VidaCompiler *compiler) {
    int loop_start = compiler->fn->chunk->count;
    expression(compiler);

    int exit_jump = emit_jump(compiler, OP_JUMP_IF_FALSE_POP);

    skip_newlines(compiler);
    consume(compiler, TOK_LEFT_BRACE, "Expect '{' before while body.");
    begin_scope(compiler);
    block(compiler);
    end_scope(compiler);

    emit_loop(compiler, loop_start);
    patch_jump(compiler, exit_jump);
}

static void synchronize(VidaCompiler *compiler) {
    compiler->panic_mode = false;
    while (compiler->current.type != TOK_EOF) {
        if (compiler->previous.type == TOK_SEMICOLON || compiler->previous.type == TOK_NEWLINE) return;
        switch (compiler->current.type) {
            case TOK_LET:
            case TOK_FN:
            case TOK_IF:
            case TOK_WHILE:
            case TOK_PRINT:
            case TOK_RETURN:
            case TOK_LEFT_BRACE:
                return;
            default:
                ; // Do nothing.
        }
        advance(compiler);
    }
}

static void declaration(VidaCompiler *compiler) {
    skip_newlines(compiler);
    if (check(compiler, TOK_EOF)) return;
    
    if (match(compiler, TOK_LET)) {
        let_declaration(compiler);
    } else if (match(compiler, TOK_FN)) {
        fn_declaration(compiler);
    } else {
        statement(compiler);
    }
    if (compiler->panic_mode) synchronize(compiler);
}

static void statement(VidaCompiler *compiler) {
    if (match(compiler, TOK_PRINT)) {
        print_statement(compiler);
    } else if (match(compiler, TOK_RETURN)) {
        return_statement(compiler);
    } else if (match(compiler, TOK_IF)) {
        if_statement(compiler);
    } else if (match(compiler, TOK_WHILE)) {
        while_statement(compiler);
    } else if (match(compiler, TOK_LEFT_BRACE)) {
        begin_scope(compiler);
        block(compiler);
        end_scope(compiler);
    } else {
        expr_statement(compiler);
    }
}

// --- Main compile ---
bool vida_compile(const char *source, VidaChunk *chunk) {
    VidaCompiler compiler;
    vida_lexer_init(&compiler.lexer, source);
    compiler.had_error = false;
    compiler.panic_mode = false;
    compiler.global_count = 0;
    compiler.fused_stmt = false;
    compiler.allow_stmt_fusion = false;
    compiler.assign_depth = 0;
    compiler.func_depth = 0;

    // Root context: the script itself. Locals/scope live here (top-level
    // `let` is global because scope_depth stays 0 at this level).
    FuncCompiler *root = &compiler.funcs[compiler.func_depth++];
    root->enclosing = NULL;
    root->local_count = 0;
    root->scope_depth = 0;
    root->chunk = chunk;
    root->func = NULL;
    root->arity = 0;
    compiler.fn = root;

    // Pre-register natives in canonical table order (marked used: they are
    // always available, never "unused"). The VM registers the same order.
    for (int i = 0; i < vida_native_count() && i < VIDA_MAX_GLOBALS; i++) {
        const char *name = vida_native_name(i);
        compiler.globals[compiler.global_count].start = name;
        compiler.globals[compiler.global_count].length = (int)strlen(name);
        compiler.globals[compiler.global_count].line = 0;
        compiler.globals[compiler.global_count].used = true;
        compiler.global_count++;
    }
    
    advance(&compiler);
    skip_newlines(&compiler);
    while (!match(&compiler, TOK_EOF)) {
        declaration(&compiler);
        skip_newlines(&compiler);
    }

    if (!compiler.had_error) {
        // Unused globals are a warning, not an error: top-level names are a
        // program's surface (and a future REPL defines-then-uses across lines).
        for (int i = 0; i < compiler.global_count; i++) {
            VidaGlobalName *g = &compiler.globals[i];
            if (!g->used && !(g->length == 1 && g->start[0] == '_')) {
                fprintf(stderr, "[line %d] Warning: unused global '%.*s' (use it or remove it)\n",
                        g->line, g->length, g->start);
            }
        }
    }

    emit_byte(&compiler, OP_HALT);
    return !compiler.had_error;
}
