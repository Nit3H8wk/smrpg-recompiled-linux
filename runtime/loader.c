// NSO loading, relocation, and symbol resolution.
#include <stdlib.h>
#include <string.h>

#include "rt.h"

Module g_modules[4];
int g_nmodules;

enum { DT_NULL = 0, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
       DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28 };
enum { R_ABS64 = 257, R_GLOB_DAT = 1025, R_JUMP_SLOT = 1026, R_RELATIVE = 1027 };

typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Elf64Sym;
typedef struct { uint64_t off, info; int64_t addend; } Elf64Rela;

size_t rt_lz4_decompress(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen) {  // (LZ4 block; also shader_prep.c)
    const uint8_t *s = src, *se = src + srclen;
    uint8_t *d = dst, *de = dst + dstlen;
    while (s < se) {
        uint8_t tok = *s++;
        size_t lit = tok >> 4;
        if (lit == 15) { uint8_t b; do { b = *s++; lit += b; } while (b == 255); }
        if (d + lit > de || s + lit > se) break;
        memcpy(d, s, lit);
        d += lit; s += lit;
        if (s >= se) break;
        size_t off = s[0] | (s[1] << 8);
        s += 2;
        size_t ml = tok & 15;
        if (ml == 15) { uint8_t b; do { b = *s++; ml += b; } while (b == 255); }
        ml += 4;
        uint8_t *m = d - off;
        if (m < dst || d + ml > de) break;
        while (ml--) *d++ = *m++;
    }
    return d - dst;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*len);
    fread(p, 1, *len, f);
    fclose(f);
    return p;
}

static void load_nso(Module *m, const char *path, uint64_t base) {
    size_t len;
    uint8_t *raw = read_file(path, &len);
    if (!raw || memcmp(raw, "NSO0", 4)) FATAL("cannot load %s\n", path);
    uint32_t flags = *(uint32_t *)(raw + 0xC);
    uint32_t seg[3][3];  // file_off, mem_off, size
    for (int i = 0; i < 3; i++) memcpy(seg[i], raw + 0x10 + i * 0x10, 12);
    uint32_t bss = *(uint32_t *)(raw + 0x3C);
    uint64_t size = (seg[2][1] + seg[2][2] + bss + 0x3FFF) & ~0x3FFFull;
    plat_commit(base, size);  // inside the guest window: zero-filled, read/write
    for (int i = 0; i < 3; i++) {
        uint32_t csize = *(uint32_t *)(raw + 0x60 + i * 4);
        uint8_t *dst = (uint8_t *)base + seg[i][1];
        if (flags & (1 << i)) {
            size_t got = rt_lz4_decompress(raw + seg[i][0], csize, dst, seg[i][2]);
            if (got != seg[i][2]) FATAL("lz4 %s seg %d: %zu != %u\n", path, i, got, seg[i][2]);
        } else {
            memcpy(dst, raw + seg[i][0], seg[i][2]);
        }
    }
    free(raw);
    m->base = base;
    m->size = size;
    m->text_off = seg[0][1]; m->text_size = seg[0][2];
    m->ro_off = seg[1][1];   m->ro_size = seg[1][2];
    m->data_off = seg[2][1]; m->data_size = seg[2][2] + bss;

    uint32_t mod0 = *(uint32_t *)(base + 4);
    uint64_t mod = base + mod0;
    m->dynamic = mod + *(int32_t *)(mod + 4);
    for (uint64_t *d = (uint64_t *)m->dynamic; d[0] != DT_NULL; d += 2) {
        switch (d[0]) {
        case DT_SYMTAB: m->symtab = base + d[1]; break;
        case DT_STRTAB: m->strtab = base + d[1]; break;
        case DT_HASH: m->nsyms = *(uint32_t *)(base + d[1] + 4); break;
        case 12: m->init = base + d[1]; break;  // DT_INIT
        case 13: m->fini = base + d[1]; break;  // DT_FINI
        case DT_INIT_ARRAY: m->init_array = base + d[1]; break;
        case DT_INIT_ARRAYSZ: m->init_array_sz = d[1]; break;
        case DT_FINI_ARRAY: m->fini_array = base + d[1]; break;
        case DT_FINI_ARRAYSZ: m->fini_array_sz = d[1]; break;
        }
    }
    LOG("loaded %-5s at %llx size %llx syms %llu\n", m->name, base, size, m->nsyms);
}

static Elf64Sym *sym(Module *m, uint64_t i) { return (Elf64Sym *)m->symtab + i; }
static const char *symname(Module *m, Elf64Sym *s) { return (const char *)(m->strtab + s->name); }

// SMRPG: global symbol table (name → address) with the first definition in load order (main before sdk), the same as
// the linear search below but in constant time: interposition (resolve) looks up every symbol of every relocation this
// way (~32,000 in the sdk). Rebuilt on every load, after all the modules are loaded.
typedef struct { const char *name; uint64_t addr; } SymSlot;
static SymSlot *g_symtab;
static uint64_t g_symtab_mask;

static uint32_t name_hash(const char *s) {  // (FNV-1a)
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void symtab_build(void) {
    uint64_t n = 0, cap = 1024;
    for (int mi = 0; mi < g_nmodules; mi++) n += g_modules[mi].nsyms;
    while (cap < n * 2) cap <<= 1;
    free(g_symtab);
    if (!(g_symtab = calloc(cap, sizeof *g_symtab))) FATAL("no memory for the symbol table\n");
    g_symtab_mask = cap - 1;
    for (int mi = 0; mi < g_nmodules; mi++) {
        Module *m = &g_modules[mi];
        for (uint64_t i = 1; i < m->nsyms; i++) {
            Elf64Sym *s = (Elf64Sym *)m->symtab + i;
            if (!s->shndx) continue;
            const char *nm = (const char *)(m->strtab + s->name);
            uint64_t h = name_hash(nm) & g_symtab_mask;
            while (g_symtab[h].name && strcmp(g_symtab[h].name, nm)) h = (h + 1) & g_symtab_mask;
            if (!g_symtab[h].name) g_symtab[h] = (SymSlot){nm, m->base + s->value};  // (already there: the first wins)
        }
    }
}

uint64_t loader_find_symbol(const char *name) {
    if (g_symtab) {
        for (uint64_t h = name_hash(name) & g_symtab_mask; g_symtab[h].name; h = (h + 1) & g_symtab_mask)
            if (!strcmp(g_symtab[h].name, name)) return g_symtab[h].addr;
        return 0;
    }
    for (int mi = 0; mi < g_nmodules; mi++) {
        Module *m = &g_modules[mi];
        for (uint64_t i = 1; i < m->nsyms; i++) {
            Elf64Sym *s = sym(m, i);
            if (s->shndx && !strcmp(symname(m, s), name)) return m->base + s->value;
        }
    }
    return 0;
}

// Prefix match: sdk signatures differ between games (nn::init::Start takes an extra
// callback on SMRPG's sdk 14.3.1: _ZN2nn4init5StartEmmPFvvES2_S2_), so match by prefix.
uint64_t loader_find_symbol_prefix(const char *prefix) {
    size_t plen = strlen(prefix);
    for (int mi = 0; mi < g_nmodules; mi++) {
        Module *m = &g_modules[mi];
        for (uint64_t i = 1; i < m->nsyms; i++) {
            Elf64Sym *s = sym(m, i);
            if (s->shndx && !strncmp(symname(m, s), prefix, plen)) return m->base + s->value;
        }
    }
    return 0;
}

const char *loader_symbol_at(uint64_t addr, uint64_t *offset) {
    const char *best = NULL;
    uint64_t best_addr = 0;
    for (int mi = 0; mi < g_nmodules; mi++) {
        Module *m = &g_modules[mi];
        if (addr < m->base || addr >= m->base + m->size) continue;
        for (uint64_t i = 1; i < m->nsyms; i++) {
            Elf64Sym *s = sym(m, i);
            uint64_t a = m->base + s->value;
            if (s->shndx && (s->info & 0xF) == 2 && a <= addr && a > best_addr) {
                best = symname(m, s);
                best_addr = a;
            }
        }
    }
    if (offset) *offset = addr - best_addr;
    return best;
}

static int interposed;  // (per module, for the log) own symbols bound to another module's definition

static uint64_t resolve(Module *m, uint32_t symidx) {
    Elf64Sym *s = sym(m, symidx);
    const char *name = symname(m, s);
    // SMRPG: like the Switch's rtld (and ELF by default), a symbol the module defines is also looked up in all the
    // modules in load order, and main's definition wins. That way Unity's operator new/delete (global in main) also
    // replace the SDK's weak ones inside the sdk: before, the sdk's std::string used the SDK's heap to free memory that
    // Unity's allocator had allocated, and CentralHeap::Free stopped the game (nnmem_nlib_debug_break). Only a local
    // symbol or one with non-default visibility binds to its own module.
    if (s->shndx) {
        uint64_t own = m->base + s->value;
        if ((s->info >> 4) == 0 || (s->other & 3)) return own;
        uint64_t a = loader_find_symbol(name);
        if (a && a != own) interposed++;
        return a ? a : own;
    }
    HostFn impl = hle_lookup_impl(name);
    if (impl) return hle_register(name, impl);  // host implementation wins over the guest SDK's
    uint64_t a = loader_find_symbol(name);
    if (a) return a;
    if ((s->info >> 4) == 2) return 0;  // weak undefined
    return hle_register(name, NULL);
}

static void relocate(Module *m) {
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0;
    for (uint64_t *d = (uint64_t *)m->dynamic; d[0] != DT_NULL; d += 2) {
        if (d[0] == DT_RELA) rela = m->base + d[1];
        if (d[0] == DT_RELASZ) relasz = d[1];
        if (d[0] == DT_JMPREL) jmprel = m->base + d[1];
        if (d[0] == DT_PLTRELSZ) pltsz = d[1];
    }
    int unresolved = 0;
    interposed = 0;
    for (int pass = 0; pass < 2; pass++) {
        Elf64Rela *r = (Elf64Rela *)(pass ? jmprel : rela);
        uint64_t n = (pass ? pltsz : relasz) / sizeof(Elf64Rela);
        for (uint64_t i = 0; i < n; i++) {
            uint64_t *where = (uint64_t *)(m->base + r[i].off);
            uint32_t type = (uint32_t)r[i].info, si = r[i].info >> 32;
            switch (type) {
            case R_RELATIVE: *where = m->base + r[i].addend; break;
            case R_ABS64:
            case R_GLOB_DAT:
            case R_JUMP_SLOT: {
                uint64_t s = resolve(m, si);
                *where = s ? s + r[i].addend : 0;
                if (s >= HLE_BASE && s < HLE_BASE + HLE_SIZE) unresolved++;
                break;
            }
            default: FATAL("%s: reloc type %u\n", m->name, type);
            }
        }
    }
    LOG("relocated %-5s (%d imports bound to host stubs, %d own symbols bound to an earlier module's definition)\n",
        m->name, unresolved, interposed);
}

void loader_load(const char *exefs) {
    char path[1024];
    static const struct { const char *name; uint64_t base; } mods[] = {{"main", MAIN_BASE}, {"sdk", SDK_BASE}};
    for (unsigned i = 0; i < 2; i++) {
        Module *m = &g_modules[g_nmodules++];
        m->name = mods[i].name;
        snprintf(path, sizeof path, "%s/%s", exefs, m->name);
        load_nso(m, path, mods[i].base);
        // SMRPG: a module cannot start inside the previous one (image + .bss): LAS's sdk at 0x7004000000 fell inside
        // SMRPG's main and their globals overwrote each other without any warning.
        if (i > 0 && g_modules[g_nmodules - 2].base + g_modules[g_nmodules - 2].size > m->base)
            FATAL("the modules overlap: %s ends at %llx and %s starts at %llx (runtime/rt.h SDK_BASE)",
                  g_modules[g_nmodules - 2].name,
                  (unsigned long long)(g_modules[g_nmodules - 2].base + g_modules[g_nmodules - 2].size), m->name,
                  (unsigned long long)m->base);
    }
    symtab_build();  // (SMRPG: with all the modules loaded, before relocating)
    for (int i = 0; i < g_nmodules; i++) relocate(&g_modules[i]);
}
