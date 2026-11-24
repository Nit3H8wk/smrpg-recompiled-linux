// SMRPG: nn::ro (NRO modules loaded at runtime), implemented on the host.
//
// On the Switch, nn::ro::Initialize connects to the ldr:ro service and uses the module lists that rtld prepares at
// startup (nn::ro::detail::g_pAutoLoadList, g_pManualLoadList, g_pLookupGlobalManualFunctionPointer). This runtime has
// neither that service nor rtld: those pointers stay 0 and nn::ro::InitializeWithPortName writes to address 0.
//
// SMRPG (Unity) imports these 8 nn::ro functions and its RomFS has no .nro or .nrr: it only calls Initialize and looks
// up symbols by name (Unity's [DllImport("__Internal")], like the CRIWARE plugin). It is the same thing nxnative does
// with Fire Emblem Engage (dm/host/hle_ro.c). Loading a module, looking inside one or registering an NRR fails and is
// logged, in case it ever gets called.
#include "../hle.h"

uint64_t loader_find_symbol(const char *name);

#define RO_FAIL (22u | (1u << 9))  // a failure of module ro (22): the game only checks for success

HLE_FUNC("_ZN2nn2ro10InitializeEv", ro_initialize) {
    LOG("[ro] Initialize (no ldr:ro service: symbols are looked up in the loaded modules)\n");
    c->x[0] = 0;
}

// nn::ro::LookupSymbol(uintptr_t *out, const char *name): the same way the loader links (runtime/loader.c resolve),
// first the host's version of that function, otherwise the definition in main or the sdk.
HLE_FUNC("_ZN2nn2ro12LookupSymbolEPmPKc", ro_lookup_symbol) {
    uint64_t out = c->x[0];
    const char *name = (const char *)(uintptr_t)c->x[1];
    uint64_t a = 0;
    if (name) {
        HostFn impl = hle_lookup_impl(name);
        a = impl ? hle_register(name, impl) : loader_find_symbol(name);
    }
    static int logged;
    if (!a || logged++ < 40)
        LOG("[ro] LookupSymbol %s -> %llx%s\n", name ? name : "(null)", (unsigned long long)a, a ? "" : " (not there)");
    if (!a) { c->x[0] = RO_FAIL; return; }
    if (out) *(uint64_t *)(uintptr_t)out = a;
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi", ro_load_module) {
    LOG("[ro] LoadModule: this port does not load NRO modules\n");
    c->x[0] = RO_FAIL;
}

HLE_FUNC("_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE", ro_unload_module) { (void)c; }

HLE_FUNC("_ZN2nn2ro13GetBufferSizeEPmPKv", ro_get_buffer_size) {
    if (c->x[0]) *(uint64_t *)(uintptr_t)c->x[0] = 0;
    LOG("[ro] GetBufferSize: this port does not load NRO modules\n");
    c->x[0] = RO_FAIL;
}

HLE_FUNC("_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc", ro_lookup_module_symbol) {
    LOG("[ro] LookupModuleSymbol %s: there are no NRO modules\n", c->x[2] ? (const char *)(uintptr_t)c->x[2] : "(null)");
    c->x[0] = RO_FAIL;
}

// NRRs only register hashes of modules that are never loaded here: success.
HLE_FUNC("_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", ro_register_module_info) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE", ro_unregister_module_info) { (void)c; }
