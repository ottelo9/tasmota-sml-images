// ============================================================================
// mtrc_plugin_mem.h — Fork-B plugin MODULE_MEMORY keystone for matter_c
// ============================================================================
//
// Included ONLY in the amalgamation build (`-DMTRC_PLUGIN_BUILD`), right after
// the `matter_ctx_t` typedef in matter_c.c. It re-points the file-scope mutable
// globals that a position-independent BinPlugin cannot relocate into the
// framework's per-module MODULE_MEMORY (a tiny, PIC-relocatable, jcalloc'd
// block) — while the heavy ~22 KB Matter context itself stays in PSRAM,
// reached only through the pointer kept here.
//
// Why a gettbl()-macro and not a SETMEMREGS local (gemu decision 2026-06-03):
//   The framework's usual `mem` pointer (declared by SETMEMREGS/ALLOCMEM) is
//   FUNCTION-LOCAL. matter_c touches `g.<field>` in hundreds of functions, so a
//   local would force SETMEMREGS at the top of every one. gettbl() is the
//   framework's global accessor for THIS module's MODULES_TABLE, so the macros
//   below resolve anywhere with no per-function setup. Matter is not a hot path
//   (commissioning + occasional reports), so a gettbl() per `g` access is fine.
//
// The non-plugin (built-in lib / host) build never defines MTRC_PLUGIN_BUILD,
// so it keeps the plain `static matter_ctx_t *g_ptr` — this header is inert
// there and the duplicated matter/src copy stays a faithful mirror.
// ============================================================================
#ifndef MTRC_PLUGIN_MEM_H
#define MTRC_PLUGIN_MEM_H

// matter_ctx_t is already defined at the include site (matter_c.c, right below
// its typedef). The crypto-ops struct is forward-declared for the bound-ops
// pointer that mtrc_crypto.c's `g_cr` becomes in the plugin build.
struct mtrc_crypto_ops;

typedef struct {
  matter_ctx_t                 *mtrc_ctx;  // was `static matter_ctx_t *g_ptr`  (matter_c.c)
  const struct mtrc_crypto_ops *cr;        // was `static const mtrc_crypto_ops *g_cr` (mtrc_crypto.c)
} MODULE_MEMORY;

// gettbl() → this module's MODULES_TABLE (declared in module_defines.h, which
// the amalgamation .cpp includes before the matter sources). mod_memory is the
// jcalloc'd MODULE_MEMORY allocated by ALLOCMEM at pFUNC_INIT.
#define MTRC_MEM  ((MODULE_MEMORY *)gettbl()->mod_memory)

// lvalue macro: `g_ptr = matter_special_malloc(...)` and `g_ptr->field` and the
// `#define g (*g_ptr)` indirection all keep working verbatim.
#define g_ptr     (MTRC_MEM->mtrc_ctx)

#endif // MTRC_PLUGIN_MEM_H
