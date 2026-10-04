// vida_native — built-in functions for compute-first vda.
//
// Conventions (kept uniform so numeric code is predictable):
// - Unary math takes one int/double (ints promote), returns double,
//   except abs() which preserves ints.
// - int(x): truncates toward zero. Strings must parse fully or it errors.
// - Errors are reported by returning false; the VM prints a generic message.
#include "vida_native.h"
#include "vida_vm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIDA_NAN_CHECK 0

typedef bool (*NativeImpl)(int argc, VidaValue *args, VidaValue *result);

static bool expect_args(int want, int argc) {
    return argc == want;
}

static bool as_number(VidaValue v, double *out) {
    if (v.type == VAL_INT) { *out = (double)v.as.integer; return true; }
    if (v.type == VAL_DOUBLE) { *out = v.as.number; return true; }
    return false;
}

#define UNARY_MATH(fn) \
    static bool nat_##fn(int argc, VidaValue *args, VidaValue *result) { \
        double x; \
        if (!expect_args(1, argc) || !as_number(args[0], &x)) return false; \
        *result = VIDA_DOUBLE(fn(x)); \
        return true; \
    }

UNARY_MATH(sqrt)
UNARY_MATH(sin)
UNARY_MATH(cos)
UNARY_MATH(tan)
UNARY_MATH(asin)
UNARY_MATH(acos)
UNARY_MATH(atan)
UNARY_MATH(exp)
UNARY_MATH(log)
UNARY_MATH(log10)
UNARY_MATH(floor)
UNARY_MATH(ceil)
UNARY_MATH(round)

static bool nat_pow(int argc, VidaValue *args, VidaValue *result) {
    double b, e;
    if (!expect_args(2, argc) || !as_number(args[0], &b) || !as_number(args[1], &e)) return false;
    *result = VIDA_DOUBLE(pow(b, e));
    return true;
}

static bool nat_atan2(int argc, VidaValue *args, VidaValue *result) {
    double y, x;
    if (!expect_args(2, argc) || !as_number(args[0], &y) || !as_number(args[1], &x)) return false;
    *result = VIDA_DOUBLE(atan2(y, x));
    return true;
}

static bool nat_abs(int argc, VidaValue *args, VidaValue *result) {
    if (!expect_args(1, argc)) return false;
    if (args[0].type == VAL_INT) {
        int64_t v = args[0].as.integer;
        *result = VIDA_INT(v < 0 ? -v : v);
        return true;
    }
    if (args[0].type == VAL_DOUBLE) {
        *result = VIDA_DOUBLE(fabs(args[0].as.number));
        return true;
    }
    return false;
}

static bool nat_min(int argc, VidaValue *args, VidaValue *result) {
    double a, b;
    if (!expect_args(2, argc) || !as_number(args[0], &a) || !as_number(args[1], &b)) return false;
    if (args[0].type == VAL_INT && args[1].type == VAL_INT) {
        *result = VIDA_INT(args[0].as.integer < args[1].as.integer ? args[0].as.integer : args[1].as.integer);
    } else {
        *result = VIDA_DOUBLE(a < b ? a : b);
    }
    return true;
}

static bool nat_max(int argc, VidaValue *args, VidaValue *result) {
    double a, b;
    if (!expect_args(2, argc) || !as_number(args[0], &a) || !as_number(args[1], &b)) return false;
    if (args[0].type == VAL_INT && args[1].type == VAL_INT) {
        *result = VIDA_INT(args[0].as.integer > args[1].as.integer ? args[0].as.integer : args[1].as.integer);
    } else {
        *result = VIDA_DOUBLE(a > b ? a : b);
    }
    return true;
}

static bool nat_rand(int argc, VidaValue *args, VidaValue *result) {
    (void)args;
    if (!expect_args(0, argc)) return false;
    *result = VIDA_DOUBLE((double)rand() / ((double)RAND_MAX + 1.0)); // [0, 1)
    return true;
}

static bool nat_srand(int argc, VidaValue *args, VidaValue *result) {
    if (!expect_args(1, argc) || args[0].type != VAL_INT) return false;
    srand((unsigned)args[0].as.integer);
    *result = VIDA_NIL();
    return true;
}

static bool nat_str(int argc, VidaValue *args, VidaValue *result) {
    if (!expect_args(1, argc)) return false;
    char buf[32];
    switch (args[0].type) {
        case VAL_NIL: *result = vida_value_string("nil", 3); return true;
        case VAL_BOOL:
            *result = vida_value_string(args[0].as.boolean ? "true" : "false",
                                        args[0].as.boolean ? 4 : 5);
            return true;
        case VAL_INT:
            snprintf(buf, sizeof(buf), "%lld", (long long)args[0].as.integer);
            *result = vida_value_string(buf, (int)strlen(buf));
            return true;
        case VAL_DOUBLE:
            snprintf(buf, sizeof(buf), "%g", args[0].as.number);
            *result = vida_value_string(buf, (int)strlen(buf));
            return true;
        case VAL_STRING: *result = vida_value_copy(args[0]); return true;
        case VAL_NATIVE:
            snprintf(buf, sizeof(buf), "<native %s>", args[0].as.native.name);
            *result = vida_value_string(buf, (int)strlen(buf));
            return true;
        case VAL_FUNC:
            snprintf(buf, sizeof(buf), "<fn %s>", args[0].as.func->name);
            *result = vida_value_string(buf, (int)strlen(buf));
            return true;
    }
    return false;
}

static bool nat_int(int argc, VidaValue *args, VidaValue *result) {
    if (!expect_args(1, argc)) return false;
    switch (args[0].type) {
        case VAL_INT: *result = args[0]; return true;
        case VAL_DOUBLE: *result = VIDA_INT((int64_t)args[0].as.number); return true;
        case VAL_BOOL: *result = VIDA_INT(args[0].as.boolean ? 1 : 0); return true;
        case VAL_STRING: {
            const char *s = args[0].as.string.chars;
            char *end = NULL;
            long long v = strtoll(s, &end, 10);
            if (end == s || *end != '\0') return false;
            *result = VIDA_INT((int64_t)v);
            return true;
        }
        default: return false;
    }
}

static bool nat_float(int argc, VidaValue *args, VidaValue *result) {
    if (!expect_args(1, argc)) return false;
    switch (args[0].type) {
        case VAL_DOUBLE: *result = args[0]; return true;
        case VAL_INT: *result = VIDA_DOUBLE((double)args[0].as.integer); return true;
        case VAL_BOOL: *result = VIDA_DOUBLE(args[0].as.boolean ? 1.0 : 0.0); return true;
        case VAL_STRING: {
            const char *s = args[0].as.string.chars;
            char *end = NULL;
            double v = strtod(s, &end);
            if (end == s || *end != '\0') return false;
            *result = VIDA_DOUBLE(v);
            return true;
        }
        default: return false;
    }
}

static bool nat_type(int argc, VidaValue *args, VidaValue *result) {
    if (!expect_args(1, argc)) return false;
    const char *name = "nil";
    switch (args[0].type) {
        case VAL_NIL: name = "nil"; break;
        case VAL_BOOL: name = "bool"; break;
        case VAL_INT: name = "int"; break;
        case VAL_DOUBLE: name = "float"; break;
        case VAL_STRING: name = "string"; break;
        case VAL_NATIVE: name = "native"; break;
        case VAL_FUNC: name = "func"; break;
    }
    *result = vida_value_string(name, (int)strlen(name));
    return true;
}

static bool nat_len(int argc, VidaValue *args, VidaValue *result) {
    if (!expect_args(1, argc) || args[0].type != VAL_STRING) return false;
    *result = VIDA_INT(args[0].as.string.length);
    return true;
}

static const struct {
    const char *name;
    NativeImpl fn;
} kNatives[] = {
    {"sqrt", nat_sqrt}, {"sin", nat_sin}, {"cos", nat_cos},
    {"tan", nat_tan}, {"asin", nat_asin}, {"acos", nat_acos},
    {"atan", nat_atan}, {"atan2", nat_atan2}, {"pow", nat_pow},
    {"exp", nat_exp}, {"log", nat_log}, {"log10", nat_log10},
    {"abs", nat_abs}, {"floor", nat_floor}, {"ceil", nat_ceil},
    {"round", nat_round}, {"min", nat_min}, {"max", nat_max},
    {"rand", nat_rand}, {"srand", nat_srand},
    {"str", nat_str}, {"int", nat_int}, {"float", nat_float},
    {"type", nat_type}, {"len", nat_len},
};

int vida_native_count(void) {
    return (int)(sizeof(kNatives) / sizeof(kNatives[0]));
}

const char *vida_native_name(int index) {
    return kNatives[index].name;
}

VidaNativeFn vida_native_fn(int index) {
    return kNatives[index].fn;
}
