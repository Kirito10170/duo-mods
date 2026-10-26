// SPDX-License-Identifier: GPL-3.0-or-later

// The Legend of Zelda: Tears of the Kingdom module: ABI glue.
// Supported build: 1.4.2 (update v655360), main 5CB42B1CF25469FB0635FD046453D843C18BC8AB.
// Every other build loads too, but only publishes the "update required" state (no reads).
//
// Skeleton stage: the readers (player position and layer, inventory pouch, game options) are
// still being located on the running game, so the module only gates the build and publishes
// the page state the manifest needs. Readers land as totk_*.h next to this file.
//
// Android is the target: nothing here may rely on guest calls (NCE has none), so changes to
// game state go through write_memory or button presses only.

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"

#include <atomic>
#include <cstring>
#include <string_view>

namespace {
using namespace dsmod_sdk::int_types;

constexpr u64 TitleId = UINT64_C(0x0100F2C0115B6000);
constexpr u8 BuildId[20]{0x5C, 0xB4, 0x2B, 0x1C, 0xF2, 0x54, 0x69, 0xFB, 0x06, 0x35,
                         0xFD, 0x04, 0x64, 0x53, 0xD8, 0x43, 0xC1, 0x8B, 0xC8, 0xAB};

EdenDsmodBool SupportsBuild(const char* build_id) {
    // Load on any build so an unsupported version can show its notice instead of a blank page.
    return build_id && std::strlen(build_id) >= 16 ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

struct Module {
    bool supported{};
};

void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId)
            return nullptr;
        auto* m = new Module{};
        m->supported = std::memcmp(host->build_id, BuildId, sizeof BuildId) == 0;
        return m;
    } catch (...) {
        return nullptr;
    }
}

void Destroy(void* p) {
    delete static_cast<Module*>(p);
}

void SampleCallback(void* p, const EdenDsmodHostApi* host) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    try {
        if (!p || !host)
            return;
        const auto* m = static_cast<const Module*>(p);
        host->begin_output(host->userdata);
        host->publish_i64(host->userdata, "totk.supported", m->supported ? 1 : 0);
        host->publish_i64(host->userdata, "totk.unsupported", m->supported ? 0 : 1);
        // Readers are not wired yet: the pages show their "in progress" state.
        host->publish_i64(host->userdata, "totk.ready", 0);
        host->end_output(host->userdata);
    } catch (...) {
        if (!reported.test_and_set() && host && host->log)
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, "TOTK DSMod sample callback failed");
    }
}

void TickCallback(void*, const EdenDsmodHostApi*) {}
void ConfigureCallback(void*, const EdenDsmodHostExtensions*) {}

EdenDsmodBool ActionCallback(void* p, const char* action, std::int64_t) {
    if (!p || !action)
        return EDEN_DSMOD_FALSE;
    return EDEN_DSMOD_FALSE; // no module actions yet
}

EdenDsmodBool LoadImageCallback(void*, const EdenDsmodHostApi*, const char*, void*,
                                EdenDsmodImageSink) {
    return EDEN_DSMOD_FALSE; // map tiles and icons come later, decoded from the player's romfs
}

const EdenDsmodModuleApi ModuleApi{EDEN_DSMOD_MODULE_ABI_VERSION,
                                   sizeof(EdenDsmodModuleApi),
                                   0,
                                   EDEN_DSMOD_MODULE_ABI_HASH,
                                   TitleId,
                                   "Tears of the Kingdom DSMod",
                                   EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ,
                                   &SupportsBuild,
                                   &Create,
                                   &Destroy,
                                   &SampleCallback,
                                   &TickCallback};
const EdenDsmodModuleExtensions ModuleExtensions{
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions), EDEN_DSMOD_EXT_HASH,
    &ConfigureCallback,     &ActionCallback,                   &LoadImageCallback};
} // namespace

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(ModuleExtensions)
