/*
** lblytheap.h — rv32 heap-accounting seam for the native host-Lua fast path.
** See Copyright Notice in lua.h
**
** blyt#231 (epic #230, ADR-0029/0008). Active only when BLYT_HOSTLUA_HEAP_SEAM
** is defined — the 64-bit desktop host-Lua VM. It makes `guest_heap_used` count
** at the 32-bit canonical (DIRECTION 1) so the native host-Lua leg reports the
** byte-identical figure as its wasm32 sibling.
**
** Mechanism: the luaM layer (lmem.c) knows, at every allocation, the
** rv32-equivalent request size — for typed vectors via BLYT_RV32_SIZEOF(t)
** below, for GC objects via blyt_heap_rv_gcsize() in lgc.c — and publishes it
** through blyt_hostlua_heap_rv_pending (runtime/shared/blyt_hostlua_heap.h)
** right before it calls the physical allocator. The runner's allocator reads
** that to size a separate rv32 *shadow* arena (the canonical guest_heap_used and
** 16 MB fail-point); physical bytes come from host malloc. The sizes themselves
** are generated from a real rv32 compile (blyt_lua_rv32_sizeof.h) so they cannot
** rot on a Lua bump.
*/

#ifndef lblytheap_h
#define lblytheap_h

#if defined(BLYT_HOSTLUA_HEAP_SEAM)

#include <stddef.h>

#include "blyt_hostlua_heap.h"  /* blyt_hostlua_heap_rv_pending (shared) */
#include "blyt_lua_rv32_sizeof.h" /* BLYT_RV32_SIZEOF_<T> (generated, rv32 ABI) */

/*
** Forward declarations so the _Generic below names every arm's type even in the
** TUs that expand BLYT_RV32_SIZEOF without including lparser.h (the parser-only
** Vardesc/Labeldesc): every arm type must be *declared* at each expansion site,
** though only the selected one need be complete. lparser.h redeclares these
** identically later, which C11 permits. The remaining arm types (TValue, Node,
** CallInfo, …) live in lobject.h / lstate.h, which every expansion site already
** includes.
*/
typedef union Vardesc Vardesc;
typedef struct Labeldesc Labeldesc;
typedef struct LX LX; /* lstate.h; named by the luaM_free arm for threads */

/*
** rv32 size of the element a pointer `pv` points to. Dispatched at compile time
** by the pointer's type, so it is a constant per site. Every element type the
** luaM typed macros allocate or free MUST have an arm — an unlisted type is a
** compile error (the completeness net), which is exactly the intended behaviour
** on a Lua-fork bump that adds one. Non-pointer-bearing element types (TValue,
** Node, Instruction, …) map to their own size, which already equals the host
** size; only pointer-bearing element types (T*, and structs with pointer
** members) actually shrink.
*/
/* clang-format off */
#define BLYT_RV32_SIZEOF_PV(pv) _Generic((pv), \
    TValue *:        (size_t)BLYT_RV32_SIZEOF_TValue, \
    StackValue *:    (size_t)BLYT_RV32_SIZEOF_StackValue, \
    Node *:          (size_t)BLYT_RV32_SIZEOF_Node, \
    Instruction *:   (size_t)BLYT_RV32_SIZEOF_Instruction, \
    AbsLineInfo *:   (size_t)BLYT_RV32_SIZEOF_AbsLineInfo, \
    Upvaldesc *:     (size_t)BLYT_RV32_SIZEOF_Upvaldesc, \
    LocVar *:        (size_t)BLYT_RV32_SIZEOF_LocVar, \
    Vardesc *:       (size_t)BLYT_RV32_SIZEOF_Vardesc, \
    Labeldesc *:     (size_t)BLYT_RV32_SIZEOF_Labeldesc, \
    CallInfo *:      (size_t)BLYT_RV32_SIZEOF_CallInfo, \
    TString **:      (size_t)BLYT_RV32_SIZEOF_ptr, \
    Proto **:        (size_t)BLYT_RV32_SIZEOF_ptr, \
    char *:          (size_t)BLYT_RV32_SIZEOF_char, \
    unsigned char *: (size_t)BLYT_RV32_SIZEOF_lu_byte, \
    signed char *:   (size_t)BLYT_RV32_SIZEOF_ls_byte, \
    /* whole GC-object structs freed directly via luaM_free (their variable */ \
    /* sub-arrays are freed separately): fixed rv32 header sizes. */ \
    Table *:         (size_t)BLYT_RV32_SIZEOF_Table, \
    Proto *:         (size_t)BLYT_RV32_SIZEOF_Proto, \
    UpVal *:         (size_t)BLYT_RV32_SIZEOF_UpVal, \
    LX *:            (size_t)BLYT_RV32_SIZEOF_LX)
/* clang-format on */

/* Element rv32 size from a type `t` (a null `t*` gives the pointer value). */
#define BLYT_RV32_SIZEOF(t) BLYT_RV32_SIZEOF_PV((t *)0)

/* Publish the rv32 size of the imminent physical allocation for the runner's
** shadow arena. Called by luaM immediately before each malloc/realloc frealloc. */
#define blyt_heap_publish_rv(rv) (blyt_hostlua_heap_rv_pending = (size_t)(rv))

#endif /* BLYT_HOSTLUA_HEAP_SEAM */

#endif
