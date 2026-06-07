// ============================================================================
// mtrc_plugin_libc.h — gettbl()-global libc remap for the matter_c amalgamation
// ============================================================================
//
// THE amalgamation enabler. The plugin framework (module_defines.h) remaps libc
// (malloc/calloc/free/snprintf/mem*/str*) to jumptable calls `((sig)jt[N])(...)`,
// but `jt` is a FUNCTION-LOCAL there (declared by SETMEMREGS/GET_JT = `mt->jt`).
// matter_c calls libc in ~61 functions across 16 files; adding SETMEMREGS to
// every one is infeasible. Since `mt = gettbl()` (the framework's global
// MODULES_TABLE accessor) and `jt = mt->jt`, the SAME gettbl() trick used for
// g_ptr (mtrc_plugin_mem.h) lets every libc call resolve `jt` globally — no
// per-function setup. (jt indices read from module_defines.h's j* macros.)
//
// Include AFTER module.h + module_defines.h (so their local-jt remaps exist to
// be #undef'd) and BEFORE the matter/src/*.c amalgamation. Plugin build only.
// ============================================================================
#ifndef MTRC_PLUGIN_LIBC_H
#define MTRC_PLUGIN_LIBC_H

// gettbl()->jt is `void (* const *)()`; cast per-call to the real signature.
#define MTRC_JT (gettbl()->jt)

#undef memcpy
#undef memmove
#undef memset
#undef memcmp
#undef strlen
#undef strncpy
#undef malloc
#undef calloc
#undef free
#undef realloc
#undef snprintf
#undef sprintf

// jt indices (module_defines.h): calloc=9 free=18 strlen=59 memcmp=89 memset=91
// memmove=92 strncpy=97 snprintf_P=22 realloc=189. No jmemcpy → memmove (jt[92])
// is a correct superset for non-overlapping copies.
#define memcpy(D,S,N)   (( void* (*)(void*,const void*,size_t) )         MTRC_JT[92])((D),(S),(N))
#define memmove(D,S,N)  (( void* (*)(void*,const void*,size_t) )         MTRC_JT[92])((D),(S),(N))
#define memset(D,C,N)   (( void* (*)(void*,int,size_t) )                 MTRC_JT[91])((D),(C),(N))
#define memcmp(A,B,N)   (( int   (*)(const void*,const void*,int) )      MTRC_JT[89])((A),(B),(N))
#define strlen(S)       (( uint32_t (*)(const char*) )                   MTRC_JT[59])((S))
#define strncpy(D,S,N)  (( char* (*)(char*,const char*,size_t) )         MTRC_JT[97])((D),(S),(N))
#define calloc(A,B)     (( void* (*)(size_t,size_t) )                    MTRC_JT[9])((A),(B))
#define malloc(A)       (( void* (*)(size_t,size_t) )                    MTRC_JT[9])((A),1)
#define free(P)         (( void  (*)(void*) )                            MTRC_JT[18])((P))
#define realloc(P,N)    (( void* (*)(void*,size_t) )                     MTRC_JT[189])((P),(N))
// snprintf/sprintf are variadic → jt[22] (jsnprintf_P). sprintf has no size, so
// route it through snprintf with a large bound (matter uses fixed local buffers).
#define snprintf(B,N,...)  (( void (*)(char*,size_t,const char*,...) )   MTRC_JT[22])((B),(N),##__VA_ARGS__)
#define sprintf(B,...)     (( void (*)(char*,size_t,const char*,...) )   MTRC_JT[22])((B),0x7fffffff,##__VA_ARGS__)

// matter uses `millis` ONLY as a matter_port_t HAL member (g.port.millis); it
// never calls the global millis(). The framework's `#define millis jmillis`
// clobbers the `.millis` member access (g.port.jmillis → "expected unqualified-id")
// → drop the remap entirely so the member name stays literal.
#undef millis
// strchr IS a real libc call (qrcodegen alphanumeric scan). jstrchr = jt[110];
// route it through gettbl() like the rest so `jt` resolves at file scope.
#undef strchr
#define strchr(A,B)     (( char* (*)(const char*,int) )                 MTRC_JT[110])((A),(B))

#endif // MTRC_PLUGIN_LIBC_H
