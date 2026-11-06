// The blink engine, as far as milestone 2 needs it: the memory hooks, which
// have nothing to invalidate yet, and the run loop, which there is nothing to
// run on until milestone 3 brings blink's interpreter in. See
// docs/blinkish.md.
#include "debug.h"
#include "emu/cpu.h"
#include "emu/engine.h"
#include "emu/tlb.h"

void engine_mem_init(struct mmu *mmu) {
    mmu->blink = NULL;
}

void engine_mem_destroy(struct mmu *UNUSED(mmu)) {
}

void engine_page_changed(struct mmu *UNUSED(mmu), page_t UNUSED(page)) {
}

int cpu_run_to_interrupt(struct cpu_state *UNUSED(cpu), struct tlb *UNUSED(tlb)) {
    die("the blink engine cannot run guest code yet");
}

void cpu_poke(struct cpu_state *UNUSED(cpu)) {
}
