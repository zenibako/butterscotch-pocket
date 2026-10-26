/* Branch displacements are signed 23-bit word offsets, independent of opcode. */
#include "vm.h"
#include "opword.h"
#include <assert.h>
#include <stdio.h>

static void checkOffset(uint32_t bits, uint32_t prefix) {
    OpWord word;
    int32_t expected = (int32_t)bits;
    if (bits & 0x400000u) expected -= 0x800000;
    word.value = (prefix << 23) | bits;
    assert(OpWord_branchOffset(word) == expected * 4);
}

int main(void) {
    uint32_t bits, prefix;
    for (bits = 0; bits < 0x800000u; ++bits)
        checkOffset(bits, 0x16cu); /* B opcode, including negative displacements. */
    for (prefix = 0; prefix < 512; ++prefix) {
        checkOffset(0, prefix);
        checkOffset(1, prefix);
        checkOffset(0x3fffffu, prefix);
        checkOffset(0x400000u, prefix);
        checkOffset(0x7fffffu, prefix);
    }
    puts("opword branch offsets: passed");
    return 0;
}
