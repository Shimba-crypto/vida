#include "vida_lexer.h"
#include <string.h>
#include <stdbool.h>

void vida_lexer_init(VidaLexer *lexer, const char *source) {
    lexer->source = source;
    lexer->current = source;
    lexer->line = 1;
    lexer->column = 1;
}

static bool is_at_end(VidaLexer *lexer) {
    return *lexer->current == '\0';
}

static char advance(VidaLexer *lexer) {
    lexer->current++;
    lexer->column++;
    return lexer->current[-1];
}

static char peek(VidaLexer *lexer) {
    return *lexer->current;
}

static char peek_next(VidaLexer *lexer) {
    if (is_at_end(lexer)) return '\0';
    return lexer->current[1];
}

static bool match(VidaLexer *lexer, char expected) {
    if (is_at_end(lexer)) return false;
    if (*lexer->current != expected) return false;
    lexer->current++;
    lexer->column++;
    return true;
}

static VidaToken make_token(VidaLexer *lexer, VidaTokenType type, const char *start, int start_col) {
    VidaToken token;
    token.type = type;
    token.start = start;
    token.length = (int)(lexer->current - start);
    token.line = lexer->line;
    token.column = start_col;
    return token;
}

static VidaToken error_token(VidaLexer *lexer, const char *message, const char *start, int start_col) {
    (void)lexer; (void)start;
    VidaToken token;
    token.type = TOK_ERROR;
    token.start = message;
    token.length = (int)strlen(message);
    token.line = lexer->line;
    token.column = start_col;
    return token;
}

static void skip_whitespace(VidaLexer *lexer) {
    for (;;) {
        char c = peek(lexer);
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
                advance(lexer);
                break;
            case '/':
                if (peek_next(lexer) == '/') {
                    while (peek(lexer) != '\n' && !is_at_end(lexer)) advance(lexer);
                } else if (peek_next(lexer) == '*') {
                    advance(lexer); // skip /
                    advance(lexer); // skip *
                    while (!is_at_end(lexer)) {
                        if (peek(lexer) == '*' && peek_next(lexer) == '/') {
                            advance(lexer);
                            advance(lexer);
                            break;
                        }
                        if (peek(lexer) == '\n') {
                            lexer->line++;
                            advance(lexer);
                            lexer->column = 1;
                        } else {
                            advance(lexer);
                        }
                    }
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static VidaTokenType check_keyword(const char *start, int length, const char *rest, int rest_length, VidaTokenType type, int token_length) {
    if (length == token_length && memcmp(start, rest, rest_length) == 0) {
        return type;
    }
    return TOK_IDENTIFIER;
}

static VidaTokenType identifier_type(const char *start, int length) {
    switch (start[0]) {
        case 'a': return check_keyword(start, length, "and", 3, TOK_AND, 3);
        case 'e': return check_keyword(start, length, "else", 4, TOK_ELSE, 4);
        case 'f':
            if (length > 1) {
                switch (start[1]) {
                    case 'a': return check_keyword(start, length, "false", 5, TOK_FALSE, 5);
                    case 'n': return check_keyword(start, length, "fn", 2, TOK_FN, 2);
                    case 'o': return check_keyword(start, length, "for", 3, TOK_FOR, 3);
                }
            }
            break;
        case 'i': return check_keyword(start, length, "if", 2, TOK_IF, 2);
        case 'l': return check_keyword(start, length, "let", 3, TOK_LET, 3);
        case 'n':
            if (length > 1) {
                switch (start[1]) {
                    case 'i': return check_keyword(start, length, "nil", 3, TOK_NIL, 3);
                    case 'o': return check_keyword(start, length, "not", 3, TOK_NOT, 3);
                }
            }
            break;
        case 'o': return check_keyword(start, length, "or", 2, TOK_OR, 2);
        case 'p': return check_keyword(start, length, "print", 5, TOK_PRINT, 5);
        case 'r': return check_keyword(start, length, "return", 6, TOK_RETURN, 6);
        case 't': return check_keyword(start, length, "true", 4, TOK_TRUE, 4);
        case 'w': return check_keyword(start, length, "while", 5, TOK_WHILE, 5);
    }
    return TOK_IDENTIFIER;
}

static VidaToken identifier(VidaLexer *lexer, const char *start, int start_col) {
    while (is_alpha(peek(lexer)) || is_digit(peek(lexer))) {
        advance(lexer);
    }
    int length = (int)(lexer->current - start);
    return make_token(lexer, identifier_type(start, length), start, start_col);
}

static VidaToken number(VidaLexer *lexer, const char *start, int start_col) {
    while (is_digit(peek(lexer))) {
        advance(lexer);
    }
    
    VidaTokenType type = TOK_INTEGER;
    
    if (peek(lexer) == '.' && is_digit(peek_next(lexer))) {
        type = TOK_FLOAT;
        advance(lexer); // Consume '.'
        while (is_digit(peek(lexer))) {
            advance(lexer);
        }
    }
    // Scientific notation: 1e10, 2.5e-3, 1E+6. The 'e' only counts when a
    // valid exponent follows, so `2 + e` still lexes as number + identifier.
    if (peek(lexer) == 'e' || peek(lexer) == 'E') {
        const char *save = lexer->current;
        int save_col = lexer->column;
        advance(lexer); // consume 'e'
        if (peek(lexer) == '+' || peek(lexer) == '-') advance(lexer);
        if (is_digit(peek(lexer))) {
            type = TOK_FLOAT;
            while (is_digit(peek(lexer))) advance(lexer);
        } else {
            lexer->current = save; // not an exponent; rewind
            lexer->column = save_col;
        }
    }
    return make_token(lexer, type, start, start_col);
}

static VidaToken string(VidaLexer *lexer, const char *start, int start_col) {
    while (peek(lexer) != '"' && !is_at_end(lexer)) {
        if (peek(lexer) == '\n') {
            lexer->line++;
            advance(lexer);
            lexer->column = 1;
        } else {
            advance(lexer);
        }
    }
    if (is_at_end(lexer)) {
        return error_token(lexer, "Unterminated string.", start, start_col);
    }
    advance(lexer); // The closing quote
    return make_token(lexer, TOK_STRING, start, start_col);
}

VidaToken vida_lexer_next(VidaLexer *lexer) {
    skip_whitespace(lexer);
    
    const char *start = lexer->current;
    int start_col = lexer->column;
    
    if (is_at_end(lexer)) return make_token(lexer, TOK_EOF, start, start_col);
    
    char c = advance(lexer);
    
    if (c == '\n') {
        lexer->line++;
        lexer->column = 1;
        return make_token(lexer, TOK_NEWLINE, start, start_col);
    }
    
    if (is_alpha(c)) return identifier(lexer, start, start_col);
    if (is_digit(c)) return number(lexer, start, start_col);
    
    switch (c) {
        case '(': return make_token(lexer, TOK_LEFT_PAREN, start, start_col);
        case ')': return make_token(lexer, TOK_RIGHT_PAREN, start, start_col);
        case '{': return make_token(lexer, TOK_LEFT_BRACE, start, start_col);
        case '}': return make_token(lexer, TOK_RIGHT_BRACE, start, start_col);
        case '[': return make_token(lexer, TOK_LEFT_BRACKET, start, start_col);
        case ']': return make_token(lexer, TOK_RIGHT_BRACKET, start, start_col);
        case ';': return make_token(lexer, TOK_SEMICOLON, start, start_col);
        case ':': return make_token(lexer, TOK_COLON, start, start_col);
        case ',': return make_token(lexer, TOK_COMMA, start, start_col);
        case '.': return make_token(lexer, TOK_DOT, start, start_col);
        case '%': return make_token(lexer, TOK_PERCENT, start, start_col);
        
        case '-': return make_token(lexer, match(lexer, '=') ? TOK_MINUS_EQUAL : TOK_MINUS, start, start_col);
        case '+': return make_token(lexer, match(lexer, '=') ? TOK_PLUS_EQUAL : TOK_PLUS, start, start_col);
        case '/': return make_token(lexer, match(lexer, '=') ? TOK_SLASH_EQUAL : TOK_SLASH, start, start_col);
        case '*': return make_token(lexer, match(lexer, '=') ? TOK_STAR_EQUAL : TOK_STAR, start, start_col);
        
        case '!': return make_token(lexer, match(lexer, '=') ? TOK_BANG_EQUAL : TOK_BANG, start, start_col);
        case '=': return make_token(lexer, match(lexer, '=') ? TOK_EQUAL_EQUAL : TOK_EQUAL, start, start_col);
        case '<': return make_token(lexer, match(lexer, '=') ? TOK_LESS_EQUAL : TOK_LESS, start, start_col);
        case '>': return make_token(lexer, match(lexer, '=') ? TOK_GREATER_EQUAL : TOK_GREATER, start, start_col);
        
        case '"': return string(lexer, start, start_col);
    }
    
    return error_token(lexer, "Unexpected character.", start, start_col);
}

VidaToken vida_lexer_peek(VidaLexer *lexer) {
    VidaLexer temp = *lexer;
    return vida_lexer_next(&temp);
}

const char *vida_token_type_name(VidaTokenType type) {
    switch (type) {
        case TOK_LEFT_PAREN: return "TOK_LEFT_PAREN";
        case TOK_RIGHT_PAREN: return "TOK_RIGHT_PAREN";
        case TOK_LEFT_BRACE: return "TOK_LEFT_BRACE";
        case TOK_RIGHT_BRACE: return "TOK_RIGHT_BRACE";
        case TOK_LEFT_BRACKET: return "TOK_LEFT_BRACKET";
        case TOK_RIGHT_BRACKET: return "TOK_RIGHT_BRACKET";
        case TOK_COMMA: return "TOK_COMMA";
        case TOK_SEMICOLON: return "TOK_SEMICOLON";
        case TOK_COLON: return "TOK_COLON";
        case TOK_DOT: return "TOK_DOT";
        case TOK_PLUS: return "TOK_PLUS";
        case TOK_PLUS_EQUAL: return "TOK_PLUS_EQUAL";
        case TOK_MINUS: return "TOK_MINUS";
        case TOK_MINUS_EQUAL: return "TOK_MINUS_EQUAL";
        case TOK_STAR: return "TOK_STAR";
        case TOK_STAR_EQUAL: return "TOK_STAR_EQUAL";
        case TOK_SLASH: return "TOK_SLASH";
        case TOK_SLASH_EQUAL: return "TOK_SLASH_EQUAL";
        case TOK_PERCENT: return "TOK_PERCENT";
        case TOK_EQUAL: return "TOK_EQUAL";
        case TOK_EQUAL_EQUAL: return "TOK_EQUAL_EQUAL";
        case TOK_BANG: return "TOK_BANG";
        case TOK_BANG_EQUAL: return "TOK_BANG_EQUAL";
        case TOK_LESS: return "TOK_LESS";
        case TOK_LESS_EQUAL: return "TOK_LESS_EQUAL";
        case TOK_GREATER: return "TOK_GREATER";
        case TOK_GREATER_EQUAL: return "TOK_GREATER_EQUAL";
        case TOK_IDENTIFIER: return "TOK_IDENTIFIER";
        case TOK_STRING: return "TOK_STRING";
        case TOK_INTEGER: return "TOK_INTEGER";
        case TOK_FLOAT: return "TOK_FLOAT";
        case TOK_LET: return "TOK_LET";
        case TOK_FN: return "TOK_FN";
        case TOK_IF: return "TOK_IF";
        case TOK_ELSE: return "TOK_ELSE";
        case TOK_WHILE: return "TOK_WHILE";
        case TOK_FOR: return "TOK_FOR";
        case TOK_RETURN: return "TOK_RETURN";
        case TOK_TRUE: return "TOK_TRUE";
        case TOK_FALSE: return "TOK_FALSE";
        case TOK_NIL: return "TOK_NIL";
        case TOK_PRINT: return "TOK_PRINT";
        case TOK_AND: return "TOK_AND";
        case TOK_OR: return "TOK_OR";
        case TOK_NOT: return "TOK_NOT";
        case TOK_NEWLINE: return "TOK_NEWLINE";
        case TOK_ERROR: return "TOK_ERROR";
        case TOK_EOF: return "TOK_EOF";
        default: return "UNKNOWN";
    }
}
