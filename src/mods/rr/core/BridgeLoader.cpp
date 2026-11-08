#include "BridgeLoader.hpp"

namespace rr::core {
BridgeLoad load_bridge(const std::filesystem::path& dir, const ModuleLoader& loader) {
    const auto path = dir / RR_BRIDGE_FILE_NAME;
    std::string error{};
    void* module = loader.open(path, error);

    if (module == nullptr) {
        if (!loader.exists(path)) {
            return {Availability::BridgeMissing, "not found: " + path.string()};
        }

        return {Availability::BridgeNotLoadable, path.string() + ": " + error};
    }

    const auto get_api = reinterpret_cast<rr_bridge_get_api_fn>(loader.find(module, RR_BRIDGE_ENTRY_NAME));

    if (get_api == nullptr) {
        loader.close(module);
        return {Availability::AbiMismatch, path.string() + " has no " RR_BRIDGE_ENTRY_NAME};
    }

    // Only abi_version is read before it matches: the rest of the table may have another layout.
    const auto* api = get_api();

    if (api == nullptr) {
        loader.close(module);
        return {Availability::AbiMismatch, path.string() + " returns no table"};
    }

    // The table lives in the module: closing it unmaps the table, so what is read goes first.
    if (const auto abi_version = api->abi_version; abi_version != RR_BRIDGE_ABI_VERSION) {
        loader.close(module);
        return {Availability::AbiMismatch, path.string() + " has ABI " + std::to_string(abi_version) + ", the Mod has " +
                                               std::to_string(RR_BRIDGE_ABI_VERSION)};
    }

    if ((api->flags & RR_BRIDGE_FLAG_NGX_SDK) == 0) {
        loader.close(module);
        return {Availability::BuiltWithoutSdk, path.string()};
    }

    return {Availability::Loaded, path.string(), module, api};
}

BridgeState to_bridge_state(Availability availability) {
    switch (availability) {
    case Availability::Loaded:
        return BridgeState::Loaded;
    case Availability::Available:
        return BridgeState::Ready;
    case Availability::OtherNgxConsumer:
        return BridgeState::OtherNgxConsumer;
    case Availability::BridgeMissing:
    case Availability::BridgeNotLoadable:
    case Availability::AbiMismatch:
    case Availability::BuiltWithoutSdk:
    case Availability::Unsupported:
    case Availability::SnippetMissing:
    case Availability::NgxError:
        break;
    }

    return BridgeState::Unavailable;
}

std::string_view to_string(Availability availability) {
    switch (availability) {
    case Availability::Loaded:
        return "bridge loaded; NGX starts at the first identified frame";
    case Availability::BridgeMissing:
        return "bridge missing";
    case Availability::BridgeNotLoadable:
        return "bridge present but not loadable";
    case Availability::AbiMismatch:
        return "ABI mismatch";
    case Availability::BuiltWithoutSdk:
        return "built without the NGX SDK";
    case Availability::Available:
        return "available";
    case Availability::Unsupported:
        return "no RTX GPU, or the driver does not support RR";
    case Availability::SnippetMissing:
        return "snippet missing: nvngx_dlssd.dll is not in reframework/rr/";
    case Availability::OtherNgxConsumer:
        return "another NGX consumer";
    case Availability::NgxError:
        return "NGX failed";
    }

    return "?";
}
} // namespace rr::core
