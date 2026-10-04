#include "vida_value.h"
#include "vida_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void vida_value_print(VidaValue value) {
    switch (value.type) {
        case VAL_NIL: fputs("nil", stdout); break;
        case VAL_BOOL: fputs(value.as.boolean ? "true" : "false", stdout); break;
        case VAL_INT: printf("%lld", (long long)value.as.integer); break;
        case VAL_DOUBLE: printf("%g", value.as.number); break;
        case VAL_STRING: fwrite(value.as.string.chars, 1, (size_t)value.as.string.length, stdout); break;
        case VAL_NATIVE: printf("<native %s>", value.as.native.name); break;
        case VAL_FUNC: printf("<fn %s>", value.as.func->name); break;
    }
}

void vida_value_free(VidaValue *value) {
    if (value->type == VAL_STRING) {
        free(value->as.string.chars);
        value->as.string.chars = NULL;
        value->as.string.length = 0;
        value->as.string.capacity = 0;
    } else if (value->type == VAL_FUNC) {
        vida_func_release(value->as.func);
        value->as.func = NULL;
    }
    value->type = VAL_NIL;
}

VidaValue vida_value_copy(VidaValue value) {
    if (value.type == VAL_STRING) {
        return vida_value_string(value.as.string.chars, value.as.string.length);
    }
    if (value.type == VAL_FUNC) {
        vida_func_retain(value.as.func);
    }
    return value;
}

bool vida_values_equal(VidaValue a, VidaValue b) {
    if (a.type != b.type) {
        if (a.type == VAL_INT && b.type == VAL_DOUBLE) return (double)a.as.integer == b.as.number;
        if (a.type == VAL_DOUBLE && b.type == VAL_INT) return a.as.number == (double)b.as.integer;
        return false;
    }
    switch (a.type) {
        case VAL_NIL: return true;
        case VAL_BOOL: return a.as.boolean == b.as.boolean;
        case VAL_INT: return a.as.integer == b.as.integer;
        case VAL_DOUBLE: return a.as.number == b.as.number;
        case VAL_NATIVE: return a.as.native.fn == b.as.native.fn;
        case VAL_FUNC: return a.as.func == b.as.func;
        case VAL_STRING:
            if (a.as.string.length != b.as.string.length) return false;
            return memcmp(a.as.string.chars, b.as.string.chars, a.as.string.length) == 0;
    }
    return false;
}

VidaValue vida_value_string(const char *str, int length) {
    VidaValue value;
    value.type = VAL_STRING;
    value.as.string.length = length;
    value.as.string.capacity = length + 1;
    value.as.string.chars = malloc((size_t)length + 1);
    if (value.as.string.chars) {
        memcpy(value.as.string.chars, str, length);
        value.as.string.chars[length] = '\0';
    } else {
        value.as.string.capacity = 0;
    }
    return value;
}

VidaValue vida_value_take_string(char *chars, int length) {
    VidaValue value;
    value.type = VAL_STRING;
    value.as.string.chars = chars;
    value.as.string.length = length;
    value.as.string.capacity = length + 1;
    return value;
}

VidaValue vida_value_take_string_cap(char *chars, int length, int capacity) {
    VidaValue value;
    value.type = VAL_STRING;
    value.as.string.chars = chars;
    value.as.string.length = length;
    value.as.string.capacity = capacity;
    return value;
}
