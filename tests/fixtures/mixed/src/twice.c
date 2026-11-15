#include "twice.h"

#include <stdlib.h>

/* Valid C but invalid C++ (implicit void* conversion): proves that .c files
   are compiled by the C compiler. */
int twice(int value) {
    int* slot = malloc(sizeof *slot);
    int result;
    *slot = value * 2;
    result = *slot;
    free(slot);
    return result;
}
