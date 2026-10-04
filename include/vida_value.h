#ifndef VIDA_VALUE_H
#define VIDA_VALUE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    VAL_NIL,
    VAL_BOOL,
    VAL_INT,
    VAL_DOUBLE,
    VAL_STRING,
    VAL_NATIVE,
    VAL_FUNC,
} VidaValueType;

// Self-tag so the native-fn typedef can precede the struct definition.
typedef struct VidaValue VidaValue;

// Function object (full struct in vida_vm.h). Refcounted: constants own one
// ref, every stack/global copy holds one.
typedef struct VidaFunc VidaFunc;

// Native function: args borrows caller's stack values (do not free);
// on success write *result (ownership passes to caller) and return true.
typedef bool (*VidaNativeFn)(int argc, VidaValue *args, VidaValue *result);

typedef struct VidaValue {
    VidaValueType type;
    union {
        bool boolean;
        int64_t integer;
        double number;
        struct {
            char *chars;
            int length;
            int capacity; // heap capacity (for exponential append)
        } string;
        struct {
            VidaNativeFn fn;
            const char *name; // static table name; never freed
        } native;
        VidaFunc *func; // refcounted; may be shared
    } as;
} VidaValue;

// Macros for creating values
#define VIDA_NIL()          ((VidaValue){VAL_NIL, {.integer = 0}})
#define VIDA_BOOL(val)      ((VidaValue){VAL_BOOL, {.boolean = (val)}})
#define VIDA_INT(val)       ((VidaValue){VAL_INT, {.integer = (val)}})
#define VIDA_DOUBLE(val)    ((VidaValue){VAL_DOUBLE, {.number = (val)}})

// Type checking macros
#define VIDA_IS_NIL(v)      ((v).type == VAL_NIL)
#define VIDA_IS_BOOL(v)     ((v).type == VAL_BOOL)
#define VIDA_IS_INT(v)      ((v).type == VAL_INT)
#define VIDA_IS_DOUBLE(v)   ((v).type == VAL_DOUBLE)
#define VIDA_IS_STRING(v)   ((v).type == VAL_STRING)

#define VIDA_AS_BOOL(v)     ((v).as.boolean)
#define VIDA_AS_INT(v)      ((v).as.integer)
#define VIDA_AS_DOUBLE(v)   ((v).as.number)
#define VIDA_AS_STRING(v)   ((v).as.string.chars)

void vida_value_print(VidaValue value);
void vida_value_free(VidaValue *value);
VidaValue vida_value_copy(VidaValue value);
bool vida_values_equal(VidaValue a, VidaValue b);
VidaValue vida_value_string(const char *str, int length);
// Take ownership of a heap buffer (no second copy).
VidaValue vida_value_take_string(char *chars, int length);
// Take ownership with explicit capacity (for append growth).
VidaValue vida_value_take_string_cap(char *chars, int length, int capacity);

#endif // VIDA_VALUE_H
