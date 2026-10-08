// Syriinge 0.6.0 plugin entry (same convention as P+ v3.2's own plugins and
// Brawlback-Online@project-plus-fork): _prolog(CoreApi*) runs the global constructors,
// installs our hooks and returns the plugin metadata to sy_core.
#include <sy_core.h>
#include "online.h"

namespace Syringe {
    const PluginMeta META = {
        "PPOnline",
        "pm_rollback",
        Version("0.1.0"),
        Version(SYRINGE_VERSION)};

    extern "C" {
        typedef void (*PFN_voidfunc)();
        __attribute__((section(".ctors"))) extern PFN_voidfunc _ctors[];
        __attribute__((section(".ctors"))) extern PFN_voidfunc _dtors[];

        const PluginMeta* _prolog(CoreApi* api);
        void _epilog();
        void _unresolved();
    }

    const PluginMeta* _prolog(CoreApi* api)
    {
        for (PFN_voidfunc* ctor = _ctors; *ctor; ctor++) {
            (*ctor)();
        }
        Online::install(api);
        return &META;
    }

    void _epilog()
    {
        for (PFN_voidfunc* dtor = _dtors; *dtor; dtor++) {
            (*dtor)();
        }
    }

    void _unresolved(void) {}
}
