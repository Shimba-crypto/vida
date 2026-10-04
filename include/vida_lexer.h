#ifndef VIDA_LEXER_H
#define VIDA_LEXER_H

typedef enum {
    // Single-character tokens
    TOK_LEFT_PAREN, TOK_RIGHT_PAREN,
    TOK_LEFT_BRACE, TOK_RIGHT_BRACE,
    TOK_LEFT_BRACKET, TOK_RIGHT_BRACKET,
    TOK_COMMA, TOK_SEMICOLON, TOK_COLON, TOK_DOT,

    // One or two character tokens
    TOK_PLUS, TOK_PLUS_EQUAL,
    TOK_MINUS, TOK_MINUS_EQUAL,
    TOK_STAR, TOK_STAR_EQUAL,
    TOK_SLASH, TOK_SLASH_EQUAL,
    TOK_PERCENT,
    TOK_EQUAL, TOK_EQUAL_EQUAL,
    TOK_BANG, TOK_BANG_EQUAL,
    TOK_LESS, TOK_LESS_EQUAL,
    TOK_GREATER, TOK_GREATER_EQUAL,

    // Literals
    TOK_IDENTIFIER, TOK_STRING, TOK_INTEGER, TOK_FLOAT,

    // Keywords
    TOK_LET, TOK_FN, TOK_IF, TOK_ELSE, TOK_WHILE, TOK_FOR,
    TOK_RETURN, TOK_TRUE, TOK_FALSE, TOK_NIL, TOK_PRINT,
    TOK_AND, TOK_OR, TOK_NOT,

    TOK_NEWLINE,
    TOK_ERROR, TOK_EOF
} VidaTokenType;

typedef struct {
    VidaTokenType type;
    const char *start;
    int length;
    int line;
    int column;
} VidaToken;

typedef struct {
    const char *source;
    const char *current;
    int line;
    int column;
} VidaLexer;

void vida_lexer_init(VidaLexer *lexer, const char *source);
VidaToken vida_lexer_next(VidaLexer *lexer);
VidaToken vida_lexer_peek(VidaLexer *lexer);
const char *vida_token_type_name(VidaTokenType type);

#endif // VIDA_LEXER_H
