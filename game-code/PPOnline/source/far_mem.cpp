// memcpy and memset inside the plugin.
//
// The plugin runs from the Network heap in MEM2 (docs/game-code.md section 2, "Heap budget"),
// beyond the reach of a relative branch to the game's code in MEM1, so it is built with
// -mlongcall. The compiler's own calls to memcpy and memset (struct copies, zeroing; also the
// ones it makes out of the source's memcpy/memset calls) ignore -mlongcall and come out as plain
// `bl`, which the linker would point at the DOL's (0x80004338, 0x8000443C). These definitions
// keep them inside the module. Written in assembly so that the compiler cannot turn the loops
// back into calls to themselves. `reltool.py check` fails the build on any far relative branch.
#include <types.h>

extern "C" {
    void* memcpy(void* dst, const void* src, u32 n);
    void* memset(void* dst, int c, u32 n);
}
// The name the compiler calls memset by (CodeWarrior mangling of memset(void*, u8, u32)).
extern "C" void* pponline_memset(void* dst, int c, u32 n) __asm__("memset__FPvUcUl");

extern "C" __attribute__((naked)) void* memcpy(void* dst, const void* src, u32 n)
{
    asm volatile(
        "cmpwi 5, 0\n\t"
        "beqlr\n\t"
        "mtctr 5\n\t"
        "addi 6, 3, -1\n\t"
        "addi 4, 4, -1\n\t"
        "1:\n\t"
        "lbzu 0, 1(4)\n\t"
        "stbu 0, 1(6)\n\t"
        "bdnz 1b\n\t"
        "blr\n\t");
}

extern "C" __attribute__((naked)) void* memset(void* dst, int c, u32 n)
{
    asm volatile(
        "cmpwi 5, 0\n\t"
        "beqlr\n\t"
        "mtctr 5\n\t"
        "addi 6, 3, -1\n\t"
        "1:\n\t"
        "stbu 4, 1(6)\n\t"
        "bdnz 1b\n\t"
        "blr\n\t");
}

extern "C" __attribute__((naked)) void* pponline_memset(void* dst, int c, u32 n)
{
    asm volatile(
        "cmpwi 5, 0\n\t"
        "beqlr\n\t"
        "mtctr 5\n\t"
        "addi 6, 3, -1\n\t"
        "1:\n\t"
        "stbu 4, 1(6)\n\t"
        "bdnz 1b\n\t"
        "blr\n\t");
}
