#pragma once

#include <D2RLPlugin/plugin_communication.h>
#include <cstdint>
#include <type_traits>

namespace DropLogger {
inline constexpr char PluginId[] = "container-drop-logger";
inline constexpr char BreadcrumbService[] = "breadcrumbs";
inline constexpr std::uint32_t BreadcrumbVersion = 1;

enum class BreadcrumbKind : std::uint32_t { Marker, IdentifyTestPlugin };
enum class BreadcrumbResult : std::uint32_t { Accepted, Unavailable, InvalidArgument, QueueFull };

struct Breadcrumb {
    std::uint32_t structSize = sizeof(Breadcrumb);
    BreadcrumbKind kind = BreadcrumbKind::Marker;
    std::uint64_t correlationId = 0;
    char pluginVersion[64]{};
    char action[64]{};
    char message[256]{};
};

struct BreadcrumbsV1 {
    // Pass the calling plugin's own context. The request is copied before return.
    // This function may be called from any thread while holding a service lease.
    BreadcrumbResult(__cdecl* write)(const D2RL::PluginContext* caller, const Breadcrumb* request) noexcept;
};

static_assert(std::is_trivially_copyable_v<Breadcrumb> && std::is_standard_layout_v<Breadcrumb>);
static_assert(sizeof(Breadcrumb) == 400 && sizeof(BreadcrumbsV1) == 8);
}
