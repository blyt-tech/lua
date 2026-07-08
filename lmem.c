/*
** $Id: lmem.c $
** Interface to Memory Manager
** See Copyright Notice in lua.h
*/

#define lmem_c
#define LUA_CORE

#include "lprefix.h"


#include <stddef.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"


#if defined(BLYT_HOSTLUA_HEAP_SEAM)
/*
** blyt#231: the rv32-equivalent size of the next physical allocation, published
** for the native host-Lua runner's shadow arena immediately before each
** frealloc that grows/creates a block (see lblytheap.h). Single-threaded VM, so
** a plain global written and read back-to-back is safe.
*/
size_t blyt_hostlua_heap_rv_pending = BLYT_HOSTLUA_HEAP_RV_UNSET;
#endif

#if defined(BLYT_HOSTLUA_HEAP_ACCT)
/* blyt#231: 1 when the next physical allocation is a thread's data stack /
** CallInfo (VM execution scratch the runner excludes from guest_heap_used). Set
** by the luaM stack sites, read+reset by the runner. See blyt_hostlua_heap.h. */
int blyt_hostlua_heap_stack_pending = 0;
#endif



/*
** About the realloc function:
** void *frealloc (void *ud, void *ptr, size_t osize, size_t nsize);
** ('osize' is the old size, 'nsize' is the new size)
**
** - frealloc(ud, p, x, 0) frees the block 'p' and returns NULL.
** Particularly, frealloc(ud, NULL, 0, 0) does nothing,
** which is equivalent to free(NULL) in ISO C.
**
** - frealloc(ud, NULL, x, s) creates a new block of size 's'
** (no matter 'x'). Returns NULL if it cannot create the new block.
**
** - otherwise, frealloc(ud, b, x, y) reallocates the block 'b' from
** size 'x' to size 'y'. Returns NULL if it cannot reallocate the
** block to the new size.
*/


/*
** Macro to call the allocation function.
*/
#define callfrealloc(g,block,os,ns)    ((*g->frealloc)(g->ud, block, os, ns))


/*
** When an allocation fails, it will try again after an emergency
** collection, except when it cannot run a collection.  The GC should
** not be called while the state is not fully built, as the collector
** is not yet fully initialized. Also, it should not be called when
** 'gcstopem' is true, because then the interpreter is in the middle of
** a collection step.
*/
#define cantryagain(g)	(completestate(g) && !g->gcstopem)




#if defined(EMERGENCYGCTESTS)
/*
** First allocation will fail except when freeing a block (frees never
** fail) and when it cannot try again; this fail will trigger 'tryagain'
** and a full GC cycle at every allocation.
*/
static void *firsttry (global_State *g, void *block, size_t os, size_t ns) {
  if (ns > 0 && cantryagain(g))
    return NULL;  /* fail */
  else  /* normal allocation */
    return callfrealloc(g, block, os, ns);
}
#else
#define firsttry(g,block,os,ns)    callfrealloc(g, block, os, ns)
#endif





/*
** {==================================================================
** Functions to allocate/deallocate arrays for the Parser
** ===================================================================
*/

/*
** Minimum size for arrays during parsing, to avoid overhead of
** reallocating to size 1, then 2, and then 4. All these arrays
** will be reallocated to exact sizes or erased when parsing ends.
*/
#define MINSIZEARRAY	4


void *luaM_growaux_ (lua_State *L, void *block, int nelems, int *psize,
                     unsigned size_elems, unsigned size_elems_rv, int limit,
                     const char *what) {
  void *newblock;
  int size = *psize;
  if (nelems + 1 <= size)  /* does one extra element still fit? */
    return block;  /* nothing to be done */
  if (size >= limit / 2) {  /* cannot double it? */
    if (l_unlikely(size >= limit))  /* cannot grow even a little? */
      luaG_runerror(L, "too many %s (limit is %d)", what, limit);
    size = limit;  /* still have at least one free place */
  }
  else {
    size *= 2;
    if (size < MINSIZEARRAY)
      size = MINSIZEARRAY;  /* minimum size */
  }
  lua_assert(nelems + 1 <= size && size <= limit);
  /* 'limit' ensures that multiplication will not overflow */
  newblock = luaM_saferealloc_(L, block, cast_sizet(*psize) * size_elems,
                                         cast_sizet(size) * size_elems,
                                         cast_sizet(*psize) * size_elems_rv,
                                         cast_sizet(size) * size_elems_rv);
  *psize = size;  /* update only when everything else is OK */
  return newblock;
}


/*
** In prototypes, the size of the array is also its number of
** elements (to save memory). So, if it cannot shrink an array
** to its number of elements, the only option is to raise an
** error.
*/
void *luaM_shrinkvector_ (lua_State *L, void *block, int *size,
                          int final_n, unsigned size_elem,
                          unsigned size_elem_rv) {
  void *newblock;
  size_t oldsize = cast_sizet(*size) * size_elem;
  size_t newsize = cast_sizet(final_n) * size_elem;
  lua_assert(newsize <= oldsize);
  newblock = luaM_saferealloc_(L, block, oldsize, newsize,
                               cast_sizet(*size) * size_elem_rv,
                               cast_sizet(final_n) * size_elem_rv);
  *size = final_n;
  return newblock;
}

/* }================================================================== */


l_noret luaM_toobig (lua_State *L) {
  luaG_runerror(L, "memory allocation error: block too big");
}


/*
** Free memory
*/
void luaM_free_ (lua_State *L, void *block, size_t osize, size_t osize_rv) {
  global_State *g = G(L);
  lua_assert((osize == 0) == (block == NULL));
  callfrealloc(g, block, osize, 0);
  g->GCdebt += cast(l_mem, BLYT_ACCT(osize, osize_rv));
}


/*
** In case of allocation fail, this function will do an emergency
** collection to free some memory and then try the allocation again.
*/
static void *tryagain (lua_State *L, void *block,
                       size_t osize, size_t nsize, size_t nsize_rv) {
  global_State *g = G(L);
  if (cantryagain(g)) {
    luaC_fullgc(L, 1);  /* try to free some memory... */
    /* re-publish: the emergency collection above may have run nested
    ** allocations that overwrote the pending rv32 size (blyt#231). */
    blyt_heap_publish_rv(nsize_rv);
    return callfrealloc(g, block, osize, nsize);  /* try again */
  }
  else return NULL;  /* cannot run an emergency collection */
}


/*
** Generic allocation routine.
*/
void *luaM_realloc_ (lua_State *L, void *block, size_t osize, size_t nsize,
                     size_t osize_rv, size_t nsize_rv) {
  void *newblock;
  global_State *g = G(L);
  lua_assert((osize == 0) == (block == NULL));
  blyt_heap_publish_rv(nsize_rv);  /* blyt#231: size the runner's shadow arena */
  newblock = firsttry(g, block, osize, nsize);
  if (l_unlikely(newblock == NULL && nsize > 0)) {
    newblock = tryagain(L, block, osize, nsize, nsize_rv);
    if (newblock == NULL)  /* still no memory? */
      return NULL;  /* do not update 'GCdebt' */
  }
  lua_assert((nsize == 0) == (newblock == NULL));
  g->GCdebt -= cast(l_mem, BLYT_ACCT(nsize, nsize_rv))
               - cast(l_mem, BLYT_ACCT(osize, osize_rv));
  return newblock;
}


void *luaM_saferealloc_ (lua_State *L, void *block, size_t osize,
                                       size_t nsize, size_t osize_rv,
                                       size_t nsize_rv) {
  void *newblock = luaM_realloc_(L, block, osize, nsize, osize_rv, nsize_rv);
  if (l_unlikely(newblock == NULL && nsize > 0))  /* allocation failed? */
    luaM_error(L);
  return newblock;
}


void *luaM_malloc_ (lua_State *L, size_t size, int tag, size_t size_rv) {
  if (size == 0)
    return NULL;  /* that's all */
  else {
    global_State *g = G(L);
    void *newblock;
    blyt_heap_publish_rv(size_rv);  /* blyt#231: size the runner's shadow arena */
    newblock = firsttry(g, NULL, cast_sizet(tag), size);
    if (l_unlikely(newblock == NULL)) {
      newblock = tryagain(L, NULL, cast_sizet(tag), size, size_rv);
      if (newblock == NULL)
        luaM_error(L);
    }
    g->GCdebt -= cast(l_mem, BLYT_ACCT(size, size_rv));
    return newblock;
  }
}
