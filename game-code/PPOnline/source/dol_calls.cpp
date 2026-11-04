// The game's functions the plugin calls by name, each through a stub here.
//
// The plugin runs from the Network heap in MEM2 (docs/game-code.md section 2, "Heap budget"),
// beyond the reach of a relative branch to the game's code. Until 2026-11-03 it was built with
// -mlongcall, which makes every call a four-instruction indirect one, also the calls inside the
// plugin, which a `bl` reaches: about 18 KB of the plugin. Now only these are long: a call to a
// game function by its name is a `bl` to its stub here, which has the function's name and jumps
// to `__dol_<name>`, the function's own entry in the symbol maps again under that name (the
// Makefile writes those entries into build/dol_calls.lst for elf2rel). Calls through a game
// address (a cast function pointer) are indirect as they always were. `reltool.py check` fails
// the build on a call to the game that has no stub here (a relative branch out of the plugin),
// and a stub whose function is in no map (an undefined `__dol_` symbol).
//
// A stub changes only r12 and CTR, which any call may change; not the arguments, nor CR bit 6,
// which a variadic function such as sprintf reads. Built without LTO (Makefile): plain assembly.
#define DOL(name)                                                                                  \
    asm(".section .text." #name ",\"ax\",@progbits\n"                                              \
        ".globl " #name "\n"                                                                       \
        ".type " #name ", @function\n" #name ":\n\t"                                               \
        "lis 12, __dol_" #name "@ha\n\t"                                                           \
        "addi 12, 12, __dol_" #name "@l\n\t"                                                       \
        "mtctr 12\n\t"                                                                             \
        "bctr\n"                                                                                   \
        ".size " #name ", . - " #name "\n"                                                         \
        ".previous\n");

// One per line as `DOL(symbol)`: the Makefile reads the names from here.
DOL(DCFlushRange)
DOL(OSReport)
DOL(alloc__13gfHeapManagerFQ25Heaps8HeapTypeUl)
DOL(createItem__9itManagerFP5Vec3ff6itKindUliP16soResourceModuleUciii)
DOL(free__FPv)
DOL(getHeap__13gfHeapManagerFQ25Heaps8HeapType)
DOL(getInstance__9itManagerFv)
DOL(getItemNum__9itManagerF6itKind)
DOL(getMaxFreeBlockSize__12gfMemoryPoolFv)
DOL(isCompItemKindArchive__9itManagerF6itKindUlb)
DOL(preloadItemKindArchive__9itManagerF6itKindUlQ29itArchive4Typeb)
DOL(printf__5MuMsgFUlPCce)
DOL(setAlignMode__5MuMsgFUlQ25MuMsg9AlignMode)
DOL(setFontColor__5MuMsgFUlUcUcUcUc)
DOL(setFontWidthModeAuto__5MuMsgFUl)
DOL(sprintf)
DOL(sscanf)
DOL(strcmp)
DOL(strcpy)
DOL(strlen)
DOL(strncmp)
DOL(strncpy)
