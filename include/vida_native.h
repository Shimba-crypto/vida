#ifndef VIDA_NATIVE_H
#define VIDA_NATIVE_H

#include "vida_value.h"

// Canonical built-in table. The compiler pre-registers the names in this
// order and the VM registers the implementations in the same order, so
// global slots always agree.
int vida_native_count(void);
const char *vida_native_name(int index);
VidaNativeFn vida_native_fn(int index);

#endif // VIDA_NATIVE_H
