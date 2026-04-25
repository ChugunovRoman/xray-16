#include "pch_script.h"

#include "xrEngine\XR_IOConsole.h"
#include "xrEngine\xr_ioc_cmd.h"
#include "ai_space.h"
#include "xrScriptEngine/script_callback_ex.h"

#include <cctype>

namespace
{
bool valid_command_name(pcstr name)
{
    if (!name || !name[0])
    {
        return false;
    }

    const unsigned char first = static_cast<unsigned char>(name[0]);
    if (!(std::isalpha(first) || first == '_'))
    {
        return false;
    }

    for (pcstr c = name; *c; ++c)
    {
        const unsigned char ch = static_cast<unsigned char>(*c);
        if (!(std::isalnum(ch) || ch == '_' || ch == '.'))
        {
            return false;
        }
    }

    return true;
}

class CCLuaConsoleCommand final : public IConsole_Command
{
public:
    CCLuaConsoleCommand(pcstr name, pcstr defaultValue, bool action, const luabind::object& callback)
        : IConsole_Command(name), m_name(xr_strdup(name)), m_defaultValue(defaultValue ? defaultValue : ""),
          m_currentValue(defaultValue ? defaultValue : ""), m_action(action)
    {
        cName = m_name;
        bLowerCaseArgs = false;
        bEmptyArgsHandled = m_action;
        m_callback.set(callback);
    }

    ~CCLuaConsoleCommand() override { xr_free(m_name); }

    void Execute(pcstr args) override
    {
        const pcstr safeArgs = args ? args : "";
        if (!m_action)
        {
            m_currentValue = safeArgs;
        }
        m_callback(safeArgs, cName);
    }

    void GetStatus(TStatus& S) override
    {
        if (m_action)
        {
            S[0] = 0;
            return;
        }
        xr_strcpy(S, m_currentValue.c_str());
    }

    void Info(TInfo& I) override
    {
        if (m_action)
        {
            xr_strcpy(I, "action command (executes callback)");
        }
        else
        {
            xr_strcpy(I, "state command: <string value>");
        }
    }

    void Save(IWriter* F) override
    {
        if (m_action)
        {
            return;
        }
        inherited::Save(F);
    }

    bool IsAction() const { return m_action; }

    void ApplyDefaultValue()
    {
        if (!m_action)
        {
            m_currentValue = m_defaultValue;
        }
    }

    void ApplyConfigValue(pcstr value)
    {
        if (m_action)
        {
            return;
        }

        const pcstr safeValue = value ? value : "";
        m_currentValue = safeValue;
        m_callback(safeValue, cName);
    }

private:
    using inherited = IConsole_Command;

private:
    pstr m_name{};
    xr_string m_defaultValue;
    xr_string m_currentValue;
    bool m_action = false;
    CScriptCallbackEx<void> m_callback;
};

class CConsoleLuaRegistrator
{
public:
    static CConsoleLuaRegistrator& Instance()
    {
        static CConsoleLuaRegistrator instance;
        return instance;
    }

    bool Register(pcstr name, bool action, pcstr defaultValue, const luabind::object& callback)
    {
        if (!Console)
        {
            return false;
        }

        if (!valid_command_name(name))
        {
            GEnv.ScriptEngine->script_log(LuaMessageType::Error,
                "command_registrator.register: invalid command name '%s'", name ? name : "<nil>");
            return false;
        }

        if (luabind::type(callback) != LUA_TFUNCTION)
        {
            GEnv.ScriptEngine->script_log(LuaMessageType::Error,
                "command_registrator.register: callback for '%s' must be a function", name);
            return false;
        }

        const xr_string key = name;

        const auto existing = m_commands.find(key);
        if (existing == m_commands.end())
        {
            if (Console->GetCommand(name))
            {
                GEnv.ScriptEngine->script_log(LuaMessageType::Error,
                    "command_registrator.register: command '%s' already exists in C++ registry", name);
                return false;
            }
        }
        else
        {
            if (Console)
            {
                Console->RemoveCommand(existing->second.get());
            }
            m_commands.erase(existing);
        }

        auto command = xr_make_unique<CCLuaConsoleCommand>(name, defaultValue ? defaultValue : "", action, callback);
        CCLuaConsoleCommand* commandPtr = command.get();

        Console->AddCommand(commandPtr);
        m_commands.emplace(key, std::move(command));

        if (!action)
        {
            shared_str pending;
            if (Console->ConsumePendingConfigCommandValue(name, pending))
            {
                commandPtr->ApplyConfigValue(pending.c_str());
            }
            else
            {
                commandPtr->ApplyDefaultValue();
            }
        }

        EnsureScriptResetSubscription();
        return true;
    }

    bool Unregister(pcstr name)
    {
        if (!name)
        {
            return false;
        }

        const auto it = m_commands.find(name);
        if (it == m_commands.end())
        {
            return false;
        }

        if (Console)
        {
            Console->RemoveCommand(it->second.get());
        }
        m_commands.erase(it);
        return true;
    }

    bool Exists(pcstr name) const
    {
        if (!name)
        {
            return false;
        }

        return m_commands.find(name) != m_commands.end();
    }

    void Clear()
    {
        if (Console)
        {
            for (const auto& commandEntry : m_commands)
            {
                IConsole_Command::TStatus value;
                commandEntry.second->GetStatus(value);
                if (value[0] != 0)
                {
                    Console->SetPendingConfigCommandValue(commandEntry.second->Name(), value);
                }
                Console->RemoveCommand(commandEntry.second.get());
            }
        }
        m_commands.clear();
    }

    void OnScriptEngineReset()
    {
        Clear();
        m_resetCallbackCid = CEventNotifierCallback::INVALID_CID;
    }

private:
    class CScriptResetCb final : public CEventNotifierCallbackWithCid
    {
    public:
        CScriptResetCb(CID cid) : CEventNotifierCallbackWithCid(cid) {}

        void ProcessEvent() override
        {
            CConsoleLuaRegistrator::Instance().OnScriptEngineReset();
            ai().Unsubscribe(GetCid(), CAI_Space::EVENT_SCRIPT_ENGINE_RESET);
        }
    };

private:
    void EnsureScriptResetSubscription()
    {
        if (m_resetCallbackCid != CEventNotifierCallback::INVALID_CID)
        {
            return;
        }

        if (!g_ai_space)
        {
            return;
        }

        m_resetCallbackCid = ai().template Subscribe<CScriptResetCb>(CAI_Space::EVENT_SCRIPT_ENGINE_RESET);
    }

private:
    xr_map<xr_string, xr_unique_ptr<CCLuaConsoleCommand>> m_commands;
    CEventNotifierCallback::CID m_resetCallbackCid = CEventNotifierCallback::INVALID_CID;
};

bool register_lua_command(pcstr name, const luabind::object& callback)
{
    return CConsoleLuaRegistrator::Instance().Register(name, false, "", callback);
}

bool register_lua_command_with_options(pcstr name, const luabind::object& callback, const luabind::object& options)
{
    bool action = false;
    xr_string defaultValue;

    if (luabind::type(options) != LUA_TNIL)
    {
        if (luabind::type(options) != LUA_TTABLE)
        {
            GEnv.ScriptEngine->script_log(LuaMessageType::Error,
                "command_registrator.register('%s'): options must be a table", name ? name : "<nil>");
            return false;
        }

        const luabind::object actionValue = options["action"];
        if (luabind::type(actionValue) == LUA_TBOOLEAN)
        {
            action = luabind::object_cast<bool>(actionValue);
        }

        const luabind::object defaultValueField = options["defaultValue"];
        if (luabind::type(defaultValueField) == LUA_TSTRING)
        {
            defaultValue = luabind::object_cast<pcstr>(defaultValueField);
        }
    }

    return CConsoleLuaRegistrator::Instance().Register(name, action, defaultValue.c_str(), callback);
}

bool unregister_lua_command(pcstr name) { return CConsoleLuaRegistrator::Instance().Unregister(name); }

bool exists_lua_command(pcstr name) { return CConsoleLuaRegistrator::Instance().Exists(name); }

void clear_lua_commands() { CConsoleLuaRegistrator::Instance().Clear(); }

class command_registrator
{
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

void command_registrator::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState, "command_registrator")
    [
        def("register", &register_lua_command),
        def("register", &register_lua_command_with_options),
        def("unregister", &unregister_lua_command),
        def("exists", &exists_lua_command),
        def("clear", &clear_lua_commands)
    ];
}
} // namespace
