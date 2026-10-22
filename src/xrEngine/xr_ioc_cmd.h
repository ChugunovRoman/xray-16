#pragma once

#define CMD0(cls)                     \
    {                                 \
        static cls x##cls();          \
        Console->AddCommand(&x##cls); \
    }
#define CMD1(cls, p1)                 \
    {                                 \
        static cls x##cls(p1);        \
        Console->AddCommand(&x##cls); \
    }
#define CMD2(cls, p1, p2)             \
    {                                 \
        static cls x##cls(p1, p2);    \
        Console->AddCommand(&x##cls); \
    }
#define CMD3(cls, p1, p2, p3)          \
    {                                  \
        static cls x##cls(p1, p2, p3); \
        Console->AddCommand(&x##cls);  \
    }
#define CMD4(cls, p1, p2, p3, p4)          \
    {                                      \
        static cls x##cls(p1, p2, p3, p4); \
        Console->AddCommand(&x##cls);      \
    }

#include "xrCore/xr_token.h"

extern ENGINE_API bool renderer_allow_override; // allows to change renderer setting

class ENGINE_API IConsole_Command
{
public:
    friend class CConsole;
    typedef char TInfo[256];
    typedef char TStatus[256];
    typedef xr_vector<shared_str> vecTips;
    typedef xr_vector<shared_str> vecLRU;

protected:
    pcstr cName;
    bool bEnabled;
    bool bLowerCaseArgs;
    bool bEmptyArgsHandled;
    bool bAuto; // auto mode of an auto-capable command, see IsAuto()

    vecLRU m_LRU;

    enum
    {
        LRU_MAX_COUNT = 10
    };

    IC bool EQ(pcstr S1, pcstr S2) { return xr_strcmp(S1, S2) == 0; }
    static bool IsAutoArg(pcstr args) { return args && xr_stricmp(args, "auto") == 0; }

public:
    IConsole_Command(pcstr N)
        : cName(N), bEnabled(true), bLowerCaseArgs(false), bEmptyArgsHandled(false), bAuto(false)
    {
        m_LRU.reserve(LRU_MAX_COUNT + 1);
        m_LRU.clear();
    }
    virtual ~IConsole_Command()
    {
        if (Console)
            Console->RemoveCommand(this);
    }

    pcstr Name() { return cName; }
    void InvalidSyntax();
    virtual void Execute(pcstr args) = 0;
    virtual void GetStatus(TStatus& S) { S[0] = 0; }
    virtual void Info(TInfo& I) { xr_strcpy(I, "(no arguments)"); }
    virtual void Save(IWriter* F)
    {
        TStatus S;
        GetStatus(S);
        if (S[0])
            F->w_printf("%s %s\r\n", cName, S);
    }

    virtual void fill_tips(vecTips& tips, u32 /*mode*/) { add_LRU_to_tips(tips); }
    // vecLRU& LRU () { return m_LRU; }
    virtual void add_to_LRU(shared_str const& arg);
    void add_LRU_to_tips(vecTips& tips);

    // Auto mode of the quality settings (plans/optimization_mt/50-smart-engine-hw-profile.md 5.2,
    // 50d-stage2-auto-engine.md). A CCC_Integer / CCC_Float / CCC_Token given a resolver (SetAuto) accepts the
    // argument "auto": its effective value comes from the hardware policy then. Any explicit value is manual, as before.
    // The variable always holds the effective value, so its consumers (and Lua get_integer / get_float, Plugin API
    // cvar_get_int / cvar_get_float) read it as before; GetStatus (cfg_save, Lua get_string) gives "auto".
    // The commands without a resolver behave exactly as before ("auto" is just a bad value for them).
    bool IsAuto() const { return bAuto; }
    virtual bool IsAutoCapable() const { return false; }
    // Re-reads the effective value of a command in auto mode: the policy changes when the render device reports its
    // GPU (CConsole::ResolveAutoCommands). Nothing for a command in manual mode.
    virtual void ResolveAuto() {}
    // The status with the effective value instead of "auto" (Lua get_token, the log); GetStatus otherwise
    virtual void GetEffectiveStatus(TStatus& S) { GetStatus(S); }

}; // class IConsole_Command

class ENGINE_API CCC_Mask : public IConsole_Command
{
protected:
    Flags32* value;
    u32 mask;

public:
    CCC_Mask(pcstr N, Flags32* V, u32 M) : IConsole_Command(N), value(V), mask(M){};
    bool GetValue() const { return value->test(mask); }
    virtual void Execute(pcstr args)
    {
        if (EQ(args, "on"))
            value->set(mask, true);
        else if (EQ(args, "off"))
            value->set(mask, false);
        else if (EQ(args, "1"))
            value->set(mask, true);
        else if (EQ(args, "0"))
            value->set(mask, false);
        else
            InvalidSyntax();
    }
    virtual void GetStatus(TStatus& S) { xr_strcpy(S, value->test(mask) ? "on" : "off"); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "'on/off' or '1/0'"); }
    virtual void fill_tips(vecTips& tips, u32 /*mode*/)
    {
        TStatus str;
        xr_sprintf(str, sizeof(str), "%s (current) [on/off]", value->test(mask) ? "on" : "off");
        tips.push_back(str);
    }
};

class ENGINE_API CCC_ToggleMask : public IConsole_Command
{
protected:
    Flags32* value;
    u32 mask;

public:
    CCC_ToggleMask(pcstr N, Flags32* V, u32 M) : IConsole_Command(N), value(V), mask(M) { bEmptyArgsHandled = true; }
    bool GetValue() const { return value->test(mask); }
    virtual void Execute(pcstr /*args*/)
    {
        value->set(mask, !GetValue());
        TStatus S;
        strconcat(sizeof(S), S, cName, " is ", value->test(mask) ? "on" : "off");
        Log(S);
    }
    virtual void GetStatus(TStatus& S) { xr_strcpy(S, value->test(mask) ? "on" : "off"); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "'on/off' or '1/0'"); }
    virtual void fill_tips(vecTips& tips, u32 /*mode*/)
    {
        TStatus str;
        xr_sprintf(str, sizeof(str), "%s (current) [on/off]", value->test(mask) ? "on" : "off");
        tips.push_back(str);
    }
};

class ENGINE_API CCC_Token : public IConsole_Command
{
public:
    // Auto mode (IConsole_Command::IsAuto): the token id from the hardware policy; false = the policy has no value
    // (no policy, "keep the default") - the engine default is used then
    using AutoResolver = bool (*)(u32& id);

protected:
    u32* value;
    const xr_token* tokens;
    AutoResolver autoResolver{};
    u32 autoDefault{}; // the value at SetAuto time (the registration, before user.ltx): the engine default

public:
    CCC_Token(pcstr N, u32* V, const xr_token* T) : IConsole_Command(N), value(V), tokens(T){}

    // Makes the command auto-capable; call at the registration, while the variable holds the engine default. It starts
    // in auto mode: without a line of it in user.ltx (a first start, a deleted user.ltx) the hardware policy decides;
    // an explicit value from user.ltx or the console makes it manual
    void SetAuto(AutoResolver resolver)
    {
        autoResolver = resolver;
        autoDefault = *value;
        bAuto = true;
        ResolveAuto();
    }
    bool IsAutoCapable() const override { return autoResolver != nullptr; }
    void ResolveAuto() override
    {
        if (!bAuto || !autoResolver)
            return;
        u32 id = autoDefault;
        const xr_token* tok = GetToken();
        // only an id of the token list: anything else would print as "?" and be saved as such
        if (!autoResolver(id) || !tok || !get_token_name(tok, int(id))[0])
            id = autoDefault;
        *value = id;
    }

    virtual void Execute(pcstr args)
    {
        if (autoResolver && IsAutoArg(args))
        {
            bAuto = true;
            ResolveAuto();
            return;
        }
        const xr_token* tok = GetToken();
        if (!tok)
        {
            Msg("! token [%s] is null", cName);
            return;
        }
        while (tok->name)
        {
            if (xr_stricmp(tok->name, args) == 0)
            {
                *value = tok->id;
                bAuto = false;
                break;
            }
            tok++;
        }
        if (!tok->name)
            InvalidSyntax();
    }
    virtual void GetStatus(TStatus& S)
    {
        if (bAuto)
        {
            xr_strcpy(S, "auto");
            return;
        }
        GetValueStatus(S);
    }
    void GetEffectiveStatus(TStatus& S) override
    {
        if (bAuto)
            GetValueStatus(S);
        else
            GetStatus(S);
    }
    // The token name of the current value
    void GetValueStatus(TStatus& S)
    {
        const xr_token* tok = GetToken();
        while (tok->name)
        {
            if (tok->id == (int)(*value))
            {
                xr_strcpy(S, tok->name);
                return;
            }
            tok++;
        }
        xr_strcpy(S, "?");
        return;
    }
    virtual void Info(TInfo& I)
    {
        auto available_size = (std::intptr_t)std::size(I) - 1;

        I[0] = 0;
        const xr_token* tok = GetToken();
        while (tok->name)
        {
            if (I[0])
                xr_strcat(I, "/");

            const auto name_size = (std::intptr_t)xr_strlen(tok->name);
            if (name_size > available_size)
            {
                if (available_size >= 3)
                    xr_strcat(I, "...");
                break; // don't just crash there, come in
            }

            xr_strcat(I, tok->name);
            available_size -= name_size;
            tok++;
        }
    }
    virtual const xr_token* GetToken() noexcept { return tokens; }
    virtual void fill_tips(vecTips& tips, u32 /*mode*/)
    {
        TStatus str;
        bool res = false;
        const xr_token* tok = GetToken();
        while (tok->name && !res)
        {
            if (tok->id == (int)(*value))
            {
                xr_sprintf(str, sizeof(str), "%s (current%s)", tok->name, bAuto ? ", auto" : "");
                tips.push_back(str);
                res = true;
            }
            tok++;
        }
        if (!res)
        {
            tips.push_back("--- (current)");
        }
        if (autoResolver)
            tips.push_back("auto");
        tok = GetToken();
        while (tok->name)
        {
            tips.push_back(tok->name);
            tok++;
        }
    }
};

class ENGINE_API CCC_Float : public IConsole_Command
{
public:
    // Auto mode (IConsole_Command::IsAuto): the value from the hardware policy; false = the policy has no value - the
    // engine default is used then. A value out of the range of the command gives the engine default too.
    using AutoResolver = bool (*)(float& value);

protected:
    float* value;
    float min, max;
    AutoResolver autoResolver{};
    float autoDefault{}; // the value at SetAuto time (the registration, before user.ltx): the engine default

public:
    CCC_Float(pcstr N, float* V, float _min = 0, float _max = 1)
        : IConsole_Command(N), value(V), min(_min), max(_max){}
    float GetValue() const { return *value; }
    void GetBounds(float& fmin, float& fmax) const
    {
        fmin = min;
        fmax = max;
    }

    // Makes the command auto-capable; call at the registration, while the variable holds the engine default. It starts
    // in auto mode: without a line of it in user.ltx (a first start, a deleted user.ltx) the hardware policy decides;
    // an explicit value from user.ltx or the console makes it manual
    void SetAuto(AutoResolver resolver)
    {
        autoResolver = resolver;
        autoDefault = *value;
        bAuto = true;
        ResolveAuto();
    }
    bool IsAutoCapable() const override { return autoResolver != nullptr; }
    void ResolveAuto() override
    {
        if (!bAuto || !autoResolver)
            return;
        float v = autoDefault;
        if (!autoResolver(v) || !(v >= min && v <= max)) // also false for NaN
            v = autoDefault;
        *value = v;
    }

    virtual void Execute(pcstr args)
    {
        if (autoResolver && IsAutoArg(args))
        {
            bAuto = true;
            ResolveAuto();
            return;
        }
        float v = float(atof(args));
        if (v < (min - EPS) || v > (max + EPS))
            InvalidSyntax();
        else
        {
            *value = v;
            bAuto = false;
        }
    }
    virtual void GetStatus(TStatus& S)
    {
        if (bAuto)
        {
            xr_strcpy(S, "auto");
            return;
        }
        GetValueStatus(S);
    }
    void GetEffectiveStatus(TStatus& S) override
    {
        if (bAuto)
            GetValueStatus(S);
        else
            GetStatus(S);
    }
    // The current value as cfg_save writes a number
    void GetValueStatus(TStatus& S)
    {
        xr_sprintf(S, sizeof(S), "%3.5f", *value);
        while (xr_strlen(S) && ('0' == S[xr_strlen(S) - 1]))
            S[xr_strlen(S) - 1] = 0;
    }
    virtual void Info(TInfo& I)
    {
        xr_sprintf(I, sizeof(I), "float value in range [%3.3f,%3.3f]%s", min, max, autoResolver ? " or 'auto'" : "");
    }
    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        TStatus str;
        xr_sprintf(str, sizeof(str), "%3.5f (current%s) [%3.3f,%3.3f]", *value, bAuto ? ", auto" : "", min, max);
        tips.push_back(str);
        if (autoResolver)
            tips.push_back("auto");
        IConsole_Command::fill_tips(tips, mode);
    }
};

class ENGINE_API CCC_Vector3 : public IConsole_Command
{
protected:
    Fvector* value;
    Fvector min, max;

public:
    CCC_Vector3(pcstr N, Fvector* V, const Fvector _min, const Fvector _max) : IConsole_Command(N), value(V)
    {
        min.set(_min);
        max.set(_max);
    };
    const Fvector GetValue() const { return *value; }
    Fvector* GetValuePtr() const { return value; }
    virtual void Execute(pcstr args)
    {
        Fvector v;
        if (3 != sscanf(args, "%f,%f,%f", &v.x, &v.y, &v.z))
        {
            if (3 != sscanf(args, "(%f,%f,%f)", &v.x, &v.y, &v.z))
            {
                InvalidSyntax();
                return;
            }
        }
        if (v.x < min.x || v.y < min.y || v.z < min.z)
        {
            InvalidSyntax();
            return;
        }
        if (v.x > max.x || v.y > max.y || v.z > max.z)
        {
            InvalidSyntax();
            return;
        }
        value->set(v);
    }
    virtual void GetStatus(TStatus& S) { xr_sprintf(S, sizeof(S), "(%f, %f, %f)", value->x, value->y, value->z); }
    virtual void Info(TInfo& I)
    {
        xr_sprintf(I, sizeof(I), "vector3 in range [%e,%e,%e]-[%e,%e,%e]", min.x, min.y, min.z, max.x, max.y, max.z);
    }
    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        TStatus str;
        xr_sprintf(str, sizeof(str), "(%e, %e, %e) (current) [(%e,%e,%e)-(%e,%e,%e)]", value->x, value->y, value->z,
            min.x, min.y, min.z, max.x, max.y, max.z);
        tips.push_back(str);
        IConsole_Command::fill_tips(tips, mode);
    }
};

class CCC_Vector4 : public IConsole_Command
{
protected:
    Fvector4* value;
    Fvector4 min, max;

public:
    CCC_Vector4(pcstr name, Fvector4* val, const Fvector4 _min, const Fvector4 _max)
        : IConsole_Command(name), value(val), min(_min), max(_max) {}

    [[nodiscard]]
    Fvector4 GetValue() const { return *value; }

    [[nodiscard]]
    Fvector4* GetValuePtr() const { return value; }

    void Execute(pcstr args) override
    {
        Fvector4 v;
        if (4 != sscanf(args, "%f,%f,%f,%f", &v.x, &v.y, &v.z, &v.w))
        {
            if (4 != sscanf(args, "(%f,%f,%f,%f)", &v.x, &v.y, &v.z, &v.w))
            {
                InvalidSyntax();
                return;
            }
        }

        if (v.x < min.x || v.y < min.y || v.z < min.z || v.w < min.w)
        {
            InvalidSyntax();
            return;
        }
        if (v.x > max.x || v.y > max.y || v.z > max.z || v.w > max.w)
        {
            InvalidSyntax();
            return;
        }
        value->set(v);
    }

    void GetStatus(TStatus& S) override
    {
        xr_sprintf(S, "(%f, %f, %f, %f)", value->x, value->y, value->z, value->w);
    }

    void Info(TInfo& I) override
    {
        xr_sprintf(I, "vector4 in range [%e,%e,%e,%e]-[%e,%e,%e,%e]", min.x, min.y, min.z, min.w, max.x,
            max.y, max.z, max.w);
    }

    void fill_tips(vecTips& tips, u32 mode) override
    {
        TStatus str;
        xr_sprintf(str, "(%e, %e, %e, %e) (current) [(%e,%e,%e,%e)-(%e,%e,%e,%e)]",
            value->x, value->y, value->z, value->w,
            min.x, min.y, min.z, min.w, max.x, max.y, max.z, max.w);
        tips.emplace_back(str);
        IConsole_Command::fill_tips(tips, mode);
    }
};

class ENGINE_API CCC_Integer : public IConsole_Command
{
public:
    // Auto mode (IConsole_Command::IsAuto): the value from the hardware policy; false = the policy has no value - the
    // engine default is used then. The result is clamped to the range of the command.
    using AutoResolver = bool (*)(int& value);

protected:
    int* value;
    int min, max;
    AutoResolver autoResolver{};
    int autoDefault{}; // the value at SetAuto time (the registration, before user.ltx): the engine default

public:
    int GetValue() const { return *value; }
    void GetBounds(int& imin, int& imax) const
    {
        imin = min;
        imax = max;
    }

    CCC_Integer(pcstr N, int* V, int _min = 0, int _max = 999) : IConsole_Command(N), value(V), min(_min), max(_max){}

    // Makes the command auto-capable; call at the registration, while the variable holds the engine default. It starts
    // in auto mode: without a line of it in user.ltx (a first start, a deleted user.ltx) the hardware policy decides;
    // an explicit value from user.ltx or the console makes it manual
    void SetAuto(AutoResolver resolver)
    {
        autoResolver = resolver;
        autoDefault = *value;
        bAuto = true;
        ResolveAuto();
    }
    bool IsAutoCapable() const override { return autoResolver != nullptr; }
    void ResolveAuto() override
    {
        if (!bAuto || !autoResolver)
            return;
        int v = autoDefault;
        if (!autoResolver(v))
            v = autoDefault;
        *value = v < min ? min : (v > max ? max : v);
    }

    virtual void Execute(pcstr args)
    {
        if (autoResolver && IsAutoArg(args))
        {
            bAuto = true;
            ResolveAuto();
            return;
        }
        int v = atoi(args);
        if (v < min || v > max)
            InvalidSyntax();
        else
        {
            *value = v;
            bAuto = false;
        }
    }
    virtual void GetStatus(TStatus& S)
    {
        if (bAuto)
            xr_strcpy(S, "auto");
        else
            xr_itoa(*value, S, 10);
    }
    void GetEffectiveStatus(TStatus& S) override
    {
        if (bAuto)
            xr_itoa(*value, S, 10);
        else
            GetStatus(S);
    }
    virtual void Info(TInfo& I)
    {
        xr_sprintf(I, sizeof(I), "integer value in range [%d,%d]%s", min, max, autoResolver ? " or 'auto'" : "");
    }
    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        TStatus str;
        xr_sprintf(str, sizeof(str), "%d (current%s) [%d,%d]", *value, bAuto ? ", auto" : "", min, max);
        tips.push_back(str);
        if (autoResolver)
            tips.push_back("auto");
        IConsole_Command::fill_tips(tips, mode);
    }
};

class ENGINE_API CCC_String : public IConsole_Command
{
protected:
    pstr value;
    int size;

public:
    CCC_String(pcstr N, pstr V, int _size = 2) : IConsole_Command(N), value(V), size(_size)
    {
        bLowerCaseArgs = false;
        R_ASSERT(V);
        R_ASSERT(size > 1);
    };

    virtual void Execute(pcstr args) { strncpy_s(value, size, args, size - 1); }
    virtual void GetStatus(TStatus& S) { xr_strcpy(S, value); }
    virtual void Info(TInfo& I) { xr_sprintf(I, sizeof(I), "string with up to %d characters", size); }
    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        tips.push_back((pcstr)value);
        IConsole_Command::fill_tips(tips, mode);
    }
};

class ENGINE_API CCC_LoadCFG : public IConsole_Command
{
public:
    virtual bool allow(pcstr /*cmd*/) { return true; }
    CCC_LoadCFG(pcstr N);
    virtual void Execute(pcstr args);
};

class ENGINE_API CCC_LoadCFG_custom : public CCC_LoadCFG
{
    string64 m_cmd;

public:
    CCC_LoadCFG_custom(pcstr cmd);
    virtual bool allow(pcstr cmd);
};
