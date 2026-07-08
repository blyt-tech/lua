/*
** $Id: lmem.h $
** Interface to Memory Manager
** See Copyright Notice in lua.h
*/

#ifndef lmem_h
#define lmem_h


#include <stddef.h>

#include "llimits.h"
#include "lua.h"

/* blyt#231: the rv32 heap-accounting seam. BLYT_RV32_SIZEOF(t)/BLYT_RV_VEC/
** BLYT_RV_ELEM give the 32-bit-canonical request size the typed macros below
** publish for the runner's shadow arena; all expand to 0 / no-ops when the seam
** is off (byte-for-byte the upstream allocator). */
#include "lblytheap.h"

/* blyt#231 stack-exclusion: on the host-Lua fast path (native seam VM + wasm),
** flag a thread's data stack / CallInfo allocations so the runner's arena keeps
** them out of guest_heap_used (VM scratch, not cart data). The rv-sizing seam
** (native only) implies it; wasm defines BLYT_HOSTLUA_HEAP_ACCT directly. Off on
** the emulated guest-lib fork (no runner to honour the flag). */
#if defined(BLYT_HOSTLUA_HEAP_SEAM) && !defined(BLYT_HOSTLUA_HEAP_ACCT)
#define BLYT_HOSTLUA_HEAP_ACCT 1
#endif

#if defined(BLYT_HOSTLUA_HEAP_ACCT)
#include "blyt_hostlua_heap.h" /* blyt_hostlua_heap_stack_pending (shared) */
/* Mark the imminent luaM allocation as VM execution scratch. Set right before the
** typed macro so it is live when the allocation reaches frealloc; the runner
** resets it on read. */
#define blyt_heap_mark_stack() (blyt_hostlua_heap_stack_pending = 1)
#else
#define blyt_heap_mark_stack() ((void)0)
#endif

#if defined(BLYT_HOSTLUA_HEAP_SEAM)
#define BLYT_RV_VEC(n, t)  ((size_t)(n) * BLYT_RV32_SIZEOF(t))
#define BLYT_RV_ELEM(t)    ((size_t)BLYT_RV32_SIZEOF(t))
/* rv32 element size from a pointer VALUE (for the free/shrink macros, which have
** the block pointer but not a bare type token). */
#define BLYT_RV_VEC_OF(n, b)  ((size_t)(n) * BLYT_RV32_SIZEOF_PV(b))
#define BLYT_RV_ELEM_OF(b)    ((size_t)BLYT_RV32_SIZEOF_PV(b))
/* Select the rv32 (canonical) vs host figure for the VM's own byte accounting
** (GCdebt / GCtotalbytes / GCmarked): rv32 under the seam so GC fires at the same
** points as the 32-bit legs; host (upstream) otherwise. */
#define BLYT_ACCT(host, rv)  (rv)
#else
#define BLYT_RV_VEC(n, t)  ((size_t)0)
#define BLYT_RV_ELEM(t)    ((size_t)0)
#define BLYT_RV_VEC_OF(n, b)  ((size_t)0)
#define BLYT_RV_ELEM_OF(b)    ((size_t)0)
#define BLYT_ACCT(host, rv)  (host)
#define blyt_heap_publish_rv(rv) ((void)(rv))
#endif


#define luaM_error(L)	luaD_throw(L, LUA_ERRMEM)


/*
** This macro tests whether it is safe to multiply 'n' by the size of
** type 't' without overflows. Because 'e' is always constant, it avoids
** the runtime division MAX_SIZET/(e).
** (The macro is somewhat complex to avoid warnings:  The 'sizeof'
** comparison avoids a runtime comparison when overflow cannot occur.
** The compiler should be able to optimize the real test by itself, but
** when it does it, it may give a warning about "comparison is always
** false due to limited range of data type"; the +1 tricks the compiler,
** avoiding this warning but also this optimization.)
*/
#define luaM_testsize(n,e)  \
	(sizeof(n) >= sizeof(size_t) && cast_sizet((n)) + 1 > MAX_SIZET/(e))

#define luaM_checksize(L,n,e)  \
	(luaM_testsize(n,e) ? luaM_toobig(L) : cast_void(0))


/*
** Computes the minimum between 'n' and 'MAX_SIZET/sizeof(t)', so that
** the result is not larger than 'n' and cannot overflow a 'size_t'
** when multiplied by the size of type 't'. (Assumes that 'n' is an
** 'int' and that 'int' is not larger than 'size_t'.)
*/
#define luaM_limitN(n,t)  \
  ((cast_sizet(n) <= MAX_SIZET/sizeof(t)) ? (n) :  \
     cast_int((MAX_SIZET/sizeof(t))))


/*
** Arrays of chars do not need any test
*/
#define luaM_reallocvchar(L,b,on,n)  \
  cast_charp(luaM_saferealloc_(L, (b), (on)*sizeof(char), (n)*sizeof(char), \
                               BLYT_RV_VEC(on, char), BLYT_RV_VEC(n, char)))

/* luaM_freemem's caller supplies the rv32 old-size (srv) — GC objects compute it
** from their type tag (blyt_heap_rv_gcsize), non-diverging blocks pass `s`. */
#define luaM_freemem(L, b, s, srv)	luaM_free_(L, (b), (s), (srv))
#define luaM_free(L, b)		luaM_free_(L, (b), sizeof(*(b)), BLYT_RV_ELEM_OF(b))
#define luaM_freearray(L, b, n)   luaM_free_(L, (b), (n)*sizeof(*(b)), BLYT_RV_VEC_OF(n, b))

#define luaM_new(L,t)		cast(t*, luaM_malloc_(L, sizeof(t), 0, BLYT_RV_ELEM(t)))
#define luaM_newvector(L,n,t)  \
	cast(t*, luaM_malloc_(L, cast_sizet(n)*sizeof(t), 0, BLYT_RV_VEC(n, t)))
#define luaM_newvectorchecked(L,n,t) \
  (luaM_checksize(L,n,sizeof(t)), luaM_newvector(L,n,t))

/* GC objects (lgc.c): the rv32 whole-object size is computed by the caller
** (blyt_heap_rv_gcsize, from the type tag + host size) since it can vary with an
** object's variable part (upvalue count, string length). */
#define luaM_newobject(L,tag,s,rvs)	luaM_malloc_(L, (s), tag, (rvs))

#define luaM_newblock(L, size)	luaM_newvector(L, size, char)

#define luaM_growvector(L,v,nelems,size,t,limit,e) \
	((v)=cast(t *, luaM_growaux_(L,v,nelems,&(size),sizeof(t), \
                         cast_uint(BLYT_RV_ELEM(t)), \
                         luaM_limitN(limit,t),e)))

#define luaM_reallocvector(L, v,oldn,n,t) \
   (cast(t *, luaM_realloc_(L, v, cast_sizet(oldn) * sizeof(t), \
                                  cast_sizet(n) * sizeof(t), \
                                  BLYT_RV_VEC(oldn, t), BLYT_RV_VEC(n, t))))

#define luaM_shrinkvector(L,v,size,fs,t) \
   ((v)=cast(t *, luaM_shrinkvector_(L, v, &(size), fs, sizeof(t), \
                                     cast_uint(BLYT_RV_ELEM(t)))))

LUAI_FUNC l_noret luaM_toobig (lua_State *L);

/* not to be called directly. The trailing rv32-size arguments (blyt#231,
** *_rv) are the 32-bit-canonical request sizes the seam accounts for GCdebt /
** the shadow arena; they are 0 and unused when the seam is off. */
LUAI_FUNC void *luaM_realloc_ (lua_State *L, void *block, size_t oldsize,
                                             size_t size, size_t osize_rv,
                                             size_t size_rv);
LUAI_FUNC void *luaM_saferealloc_ (lua_State *L, void *block, size_t oldsize,
                                                 size_t size, size_t osize_rv,
                                                 size_t size_rv);
LUAI_FUNC void luaM_free_ (lua_State *L, void *block, size_t osize,
                                         size_t osize_rv);
LUAI_FUNC void *luaM_growaux_ (lua_State *L, void *block, int nelems,
                               int *size, unsigned size_elem,
                               unsigned size_elem_rv, int limit,
                               const char *what);
LUAI_FUNC void *luaM_shrinkvector_ (lua_State *L, void *block, int *nelem,
                                    int final_n, unsigned size_elem,
                                    unsigned size_elem_rv);
LUAI_FUNC void *luaM_malloc_ (lua_State *L, size_t size, int tag, size_t size_rv);

#endif

