#ifndef EMU_ENGINE_H
#define EMU_ENGINE_H

// What the memory layer tells the CPU engine: an address space was made or
// freed, and a page's contents or mapping changed, so whatever the engine
// derived from it (asbestos's compiled blocks, blink's decoded instructions)
// is stale. One engine is built in, chosen by -Dengine.

#include "emu/mmu.h"

#if ENGINE_ASBESTOS
#include "asbestos/asbestos.h"
static inline void engine_mem_init(struct mmu *mmu) {
    mmu->asbestos = asbestos_new(mmu);
}
static inline void engine_mem_destroy(struct mmu *mmu) {
    asbestos_free(mmu->asbestos);
}
static inline void engine_page_changed(struct mmu *mmu, page_t page) {
    asbestos_invalidate_page(mmu->asbestos, page);
}
#elif ENGINE_BLINK
void engine_mem_init(struct mmu *mmu);
void engine_mem_destroy(struct mmu *mmu);
void engine_page_changed(struct mmu *mmu, page_t page);
#else
#error "no CPU engine for the ish kernel"
#endif

#endif
