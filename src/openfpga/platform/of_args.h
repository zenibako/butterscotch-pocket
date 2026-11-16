#ifndef OF_ARGS_H
#define OF_ARGS_H

#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

/* Advance only after a complete positive threshold; a following option must
 * remain available to the caller when the value is absent or malformed. */
static inline bool ofParseSlowFrameArg(int argc, char** argv, int* index, unsigned* ms) {
    if (*index + 1 >= argc) return false;
    const char* text = argv[*index + 1];
    if (text[0] < '0' || text[0] > '9') return false;
    char* end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno == ERANGE || *end != '\0' || value == 0 || value > UINT_MAX) return false;
    *ms = (unsigned) value;
    ++*index;
    return true;
}

#endif
