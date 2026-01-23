#include "pch_script.h"
#include "Checker.h"

Checker::Checker()
{
    // 
}
Checker::~Checker()
{
    // 
}

void Checker::AddToCheckLog(shared_str type, shared_str msg)
{
    auto exists = checks.find(type);
    if (exists == checks.end())
        checks[type] = {};

    auto exists2 = checks[type].find(msg);
    if (exists2 != checks[type].end())
        return;

    checks[type][msg] = true;

    xr_string discriptor_name = "check_";
    discriptor_name.append(type.c_str());
    _Trim(discriptor_name);

    xr_string path = type.c_str();
    path.append(".log");
    _Trim(path);

    string_path log_file_name;
    FS.update_path(log_file_name, "$checks$", path.c_str());

    auto hasDesc = discriptors.find(discriptor_name.c_str());
    IWriter* w = nullptr;

    if (hasDesc != discriptors.end())
        w = discriptors[discriptor_name.c_str()];
    else
        w = FS.w_open(log_file_name);
    
    if (w)
    {
        discriptors[discriptor_name.c_str()] = w;

        cpcstr s = msg.c_str();
        w->w_printf("%s\r\n", s ? s : "");
    }
    
}
void Checker::AddToDictLog(shared_str type, shared_str msg)
{
    auto exists = dicts.find(type);
    if (exists == dicts.end())
        dicts[type] = {};

    auto exists2 = dicts[type].find(msg);
    if (exists2 != dicts[type].end())
        return;

    dicts[type][msg] = true;

    xr_string discriptor_name = "dictionary_";
    discriptor_name.append(type.c_str());
    _Trim(discriptor_name);

    xr_string path = type.c_str();
    path.append(".log");
    _Trim(path);

    string_path log_file_name;
    FS.update_path(log_file_name, "$dictionaries$", path.c_str());

    auto hasDesc = discriptors.find(discriptor_name.c_str());
    IWriter* w = nullptr;

    if (hasDesc != discriptors.end())
        w = discriptors[discriptor_name.c_str()];
    else
        w = FS.w_open(log_file_name);
    
    if (w)
    {
        discriptors[discriptor_name.c_str()] = w;

        cpcstr s = msg.c_str();
        w->w_printf("%s\r\n", s ? s : "");
    }
}

void Checker::CloseAllDescriptors()
{
    for (auto I : discriptors)
        FS.w_close(I.second);

    discriptors.clear();
}
