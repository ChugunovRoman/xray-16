#include "StdAfx.h"

// Plugin API, group services: one plugin publishes an interface of its own (a struct of function pointers it
// defines), others find it by name (wiki/doc/plugins/api/services.md). The engine keeps only the pointer, the
// version and the owner: it never calls into the interface. The services of a plugin go when it stops (unload,
// crash), and every change is announced on the event bus (service_on_register / service_on_unregister) so a
// consumer drops its pointer before the library of the provider is unloaded.

#include "addon_api_services.h"
#include "addon_event_bus.h"
#include "addon_host.h"

namespace gw::addons::services
{
namespace
{
constexpr size_t kMaxServiceName = 128;

struct Service
{
    xr_string name;
    u32 version = 0;
    const void* iface = nullptr;
    const GwpPlugin* owner = nullptr;
    // Published during the init of its plugin: announced (service_on_register) only once the plugin loaded, and
    // removed silently when the init fails - nobody heard of it, nobody may call into a library about to go
    bool announced = false;
};

xr_vector<Service>* g_services = nullptr; // a handful of entries: a linear search is enough

bool CheckServicesCall(pcstr function)
{
    if (IsMainThread())
        return true;
    // Once per run: a loop calling it from another thread would flood the log
    static bool reported = false;
    if (!reported)
    {
        reported = true;
        Msg("! [services] %s called outside the main thread, ignored (reported once)", function);
    }
    return false;
}

Service* FindService(pcstr name)
{
    if (!g_services)
        return nullptr;
    for (Service& service : *g_services)
    {
        if (service.name == name)
            return &service;
    }
    return nullptr;
}

// "<owner>/<name>": an owner part and a name part, up to kMaxServiceName - 1 characters
bool ValidServiceName(pcstr name)
{
    if (!name || !*name || xr_strlen(name) >= kMaxServiceName)
        return false;
    const pcstr slash = strchr(name, '/');
    return slash && slash != name && slash[1];
}

// The owner part is the id of the calling plugin, as the keys of the data bus: a plugin publishes under its own id
bool OwnName(const GwpPlugin* self, pcstr name)
{
    const pcstr id = PluginAddonId(self);
    const size_t id_length = xr_strlen(id);
    return strncmp(name, id, id_length) == 0 && name[id_length] == '/';
}

void EmitRegister(const Service& service)
{
    const GwpValue args[] = { events::String(service.name.c_str()), events::Int(service.version) };
    events::Emit(events::EBuiltin::ServiceOnRegister, args, 2);
}

void EmitUnregister(const xr_string& name)
{
    const GwpValue args[] = { events::String(name.c_str()) };
    events::Emit(events::EBuiltin::ServiceOnUnregister, args, 1);
}

GwpResult GWP_CALL ApiServiceRegister(const GwpPlugin* self, const char* name, uint32_t version, const void* iface)
{
    if (!CheckServicesCall("service_register"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    if (!self || !iface || !version || !ValidServiceName(name))
    {
        Msg("! [plugin:%s] service_register: the name must be '%s/<name>' (up to %u characters), the version 1 or "
            "more and the interface not NULL; got '%s', version %u", PluginAddonId(self), PluginAddonId(self),
            static_cast<u32>(kMaxServiceName - 1), name ? name : "<null>", version);
        return GWP_ERROR_INVALID_ARGUMENT;
    }
    if (!OwnName(self, name))
    {
        Msg("! [plugin:%s] service_register: '%s' is not under the id of this addon ('%s/<name>')",
            PluginAddonId(self), name, PluginAddonId(self));
        return GWP_ERROR_ACCESS_DENIED;
    }
    // A plugin stopped by a crash runs on in the frames until its unload: what it registers then would point into
    // a library about to go
    if (!IsPluginRunning(self))
        return GWP_ERROR_INVALID_STATE;
    if (!g_services)
        g_services = xr_new<xr_vector<Service>>();
    const bool announce = !IsPluginLoading(self); // during init: announced once the plugin has loaded
    Service* service = FindService(name);
    if (service && service->owner != self)
        return GWP_ERROR_ACCESS_DENIED; // the name of a plugin unloaded and loaded again in one frame: not ours
    if (service)
    {
        // The same name again replaces the interface. A consumer may hold the old one: it hears the old service go
        // first, then the new one come
        const bool was_announced = service->announced;
        const xr_string old_name = service->name;
        g_services->erase(g_services->begin() + (service - g_services->data()));
        if (was_announced)
            EmitUnregister(old_name);
    }
    g_services->push_back({ name, version, iface, self, announce });
    if (IsDebugLog())
        Msg("  [services] %s registered, version %u%s", name, version, announce ? "" : " (announced after the init)");
    if (announce)
    {
        const Service copy = g_services->back(); // a handler may register another service: the vector may move
        EmitRegister(copy);
    }
    return GWP_OK;
}

GwpResult GWP_CALL ApiServiceUnregister(const GwpPlugin* self, const char* name)
{
    if (!CheckServicesCall("service_unregister"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    if (!self || !name)
        return GWP_ERROR_INVALID_ARGUMENT;
    Service* service = FindService(name);
    if (!service)
        return GWP_ERROR_NOT_FOUND;
    if (service->owner != self)
        return GWP_ERROR_ACCESS_DENIED;
    const xr_string removed = service->name;
    const bool was_announced = service->announced;
    g_services->erase(g_services->begin() + (service - g_services->data()));
    if (was_announced)
        EmitUnregister(removed);
    return GWP_OK;
}

const void* GWP_CALL ApiServiceGet(const GwpPlugin* self, const char* name, uint32_t min_version, uint32_t* out_version)
{
    if (out_version)
        *out_version = 0;
    if (!CheckServicesCall("service_get") || !self || !name)
        return nullptr;
    const Service* service = FindService(name);
    if (!service || !service->announced)
        return nullptr; // one published during the init of its plugin exists only once that plugin has loaded
    if (out_version)
        *out_version = service->version;
    return service->version >= min_version ? service->iface : nullptr;
}
} // namespace

void FillEngineApi(GwpEngineApi& api)
{
    api.service_register = &ApiServiceRegister;
    api.service_unregister = &ApiServiceUnregister;
    api.service_get = &ApiServiceGet;
}

void RemovePluginServices(const GwpPlugin* plugin)
{
    if (!g_services)
        return;
    // Taken out first, announced after: a handler may call service_get, which must not find them any more. One
    // never announced (a failed init) goes silently
    xr_vector<xr_string> removed;
    for (auto it = g_services->begin(); it != g_services->end();)
    {
        if (it->owner == plugin)
        {
            if (it->announced)
                removed.push_back(it->name);
            it = g_services->erase(it);
        }
        else
            ++it;
    }
    if (removed.empty())
        return;
    if (!IsMainThread())
    {
        Msg("! [services] %u service(s) of plugin '%s' removed outside the main thread: service_on_unregister not sent",
            static_cast<u32>(removed.size()), PluginAddonId(plugin));
        return;
    }
    for (const xr_string& name : removed)
        EmitUnregister(name);
}

void AnnouncePluginServices(const GwpPlugin* plugin)
{
    if (!g_services)
        return;
    xr_vector<Service> announced;
    for (Service& service : *g_services)
    {
        if (service.owner == plugin && !service.announced)
        {
            service.announced = true;
            announced.push_back(service);
        }
    }
    for (const Service& service : announced) // copies: a handler may register or remove services
        EmitRegister(service);
}

void Shutdown() { xr_delete(g_services); }

void PrintList()
{
    const u32 count = g_services ? static_cast<u32>(g_services->size()) : 0;
    Msg("- [services] %u service(s)", count);
    for (u32 i = 0; i < count; ++i)
    {
        const Service& service = (*g_services)[i];
        Msg("-   %-40s version %u, %s", service.name.c_str(), service.version, PluginAddonId(service.owner));
    }
}
} // namespace gw::addons::services
