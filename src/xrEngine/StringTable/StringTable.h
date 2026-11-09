//////////////////////////////////////////////////////////////////////////
// string_table.h: таблица строк, используемых в игре
//////////////////////////////////////////////////////////////////////////

#pragma once

#include "xrCommon/xr_map.h"
#include "xrCommon/xr_string.h"
#include "xrCommon/xr_vector.h"
#include "xrCore/xrstring.h"
#include "xrCore/xr_token.h"

#include <mutex>
#include <utility>

using STRING_ID = shared_str;
using STRING_VALUE = shared_str;

using STRING_TABLE_MAP = xr_map<STRING_ID, STRING_VALUE>;

struct STRING_TABLE_DATA
{
    shared_str m_fontPrefix;
    shared_str m_sLanguage;
    shared_str m_sCurrency;
    STRING_TABLE_MAP m_StringTable;
    STRING_TABLE_MAP m_tokens;
};

class ENGINE_API CStringTable final
{
public:
    void Init();
    static void Destroy();

    void replace(const STRING_ID& str_id, const STRING_VALUE& new_str);

    void register_token(const STRING_ID& key, const STRING_VALUE& value);
    void unregister_token(const STRING_ID& key);

    [[nodiscard]]
    STRING_VALUE apply_tokens(const STRING_VALUE& str) const;

    [[nodiscard]]
    STRING_VALUE translate(const STRING_ID& str_id) const;

    [[nodiscard]]
    STRING_VALUE translate(const STRING_ID& str_id, const STRING_ID& str_id2) const; // returns str_id if no translation found

    bool translate(const STRING_ID& str_id, STRING_VALUE& out) const;
    bool exist(const STRING_ID& str_id) const;

    [[nodiscard]]
    bool has_translation(const STRING_ID& str_id) const;

    void rescan();

    void ReloadLanguage();

    shared_str GetCurrentLanguage() const;
    shared_str GetCurrentFontPrefix() const;
    shared_str GetCurrency() const;
    xr_token* GetLanguagesToken() const;

    static u32 LanguageID;
    static string32 LanguageIDInLTX;

private:
    struct StringTableEntry
    {
        STRING_ID id;
        STRING_VALUE value;
        // A string a patch of the file set (XMLOverride, XMLDocument::kPatchOwnerAttr): the owner and the layer of the
        // patch, and the owners of the strings of the id it replaced in the file, in order (a list). Empty - the file's.
        xr_string owner;
        u16 layer{};
        xr_string over;
    };
    // A file of text\<lang>\ (plans/addons_api_support/09-content-addons-vfs.md 5.2): parsed in parallel, merged in
    // the order of the file system layers
    struct StringTableFile
    {
        xr_string name; // relative to text\<lang>\ (subfolders included)
        xr_string owner; // the addon of its layer, "base"
        u16 layer{};
        xr_vector<StringTableEntry> strings; // in the order of the file
    };
    void Load(StringTableFile& file) const;
    void Merge(const xr_vector<StringTableFile>& files);
    void FillLanguageToken();
    void SetLanguage();
    static STRING_VALUE ParseLine(pcstr str);

    static std::mutex pDataMutex;
    static xr_unique_ptr<STRING_TABLE_DATA> pData;
    static xr_vector<xr_token> languagesToken;
};

ENGINE_API CStringTable& StringTable();
