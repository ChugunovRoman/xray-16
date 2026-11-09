#include "stdafx.h"
#include "StringTable.h"

#include "xr_level_controller.h"

#include "xrCore/XML/XMLDocument.hpp"
#include "xrCore/ContentPatches.h"
#include "xrCore/Threading/ParallelForEach.hpp"

#include <algorithm>

constexpr pcstr OPENXRAY_XML = "openxray.xml";

CStringTable& StringTable()
{
    static CStringTable string_table;
    return string_table;
}

std::mutex CStringTable::pDataMutex;
xr_unique_ptr<STRING_TABLE_DATA> CStringTable::pData{};
u32 CStringTable::LanguageID = std::numeric_limits<u32>::max();
string32 CStringTable::LanguageIDInLTX{};
xr_vector<xr_token> CStringTable::languagesToken;

void CStringTable::Destroy()
{
    pData.reset(nullptr);

    for (auto& token : languagesToken)
    {
        auto tokenName = const_cast<char*>(token.name);
        xr_free(tokenName);
    }

    languagesToken.clear();
}

void CStringTable::rescan()
{
    if (!pData)
    {
        Destroy();
        Init();
    }
}

void CStringTable::Init()
{
    if (pData)
        return;

    ZoneScoped;

    pData = xr_make_unique<STRING_TABLE_DATA>();

    FillLanguageToken();
    SetLanguage();

    FS_FileSet fset;
    string_path files_mask;
    xr_sprintf(files_mask, "text" DELIMITER "%s" DELIMITER "*.xml", pData->m_sLanguage.c_str());
    FS.file_list(fset, "$game_config$", FS_ListFiles, files_mask);

    // plans/addons_api_support/09-content-addons-vfs.md 5.2: the files are parsed in parallel, then merged in the
    // order of the file system layers (the base first, then the layers as mounted; by the name inside a layer):
    // an id given twice takes the value of the later file, whatever thread parsed it first. The mask takes the
    // subfolders of text\<lang>\ too: a file is opened by its path relative to text\<lang>\.
    string_path language_folder;
    xr_sprintf(language_folder, "text" DELIMITER "%s" DELIMITER, pData->m_sLanguage.c_str());
    xr_fs_strlwr(language_folder); // as the index spells the names: the case stays where the file system keeps it
    const size_t language_folder_length = xr_strlen(language_folder);

    xr_vector<StringTableFile> files;
    files.reserve(fset.size());
    for (const FS_File& it : fset)
    {
        string_path full;
        FS.update_path(full, "$game_config$", it.name.c_str());
        if (xray::content_patches::IsPatchFile(full))
            continue; // mod_<name>_*.xml: XMLOverride applies it to <name>.xml, its new ids are added there
        StringTableFile& file = files.emplace_back();
        pcstr relative = it.name.c_str();
        if (0 == strncmp(relative, language_folder, language_folder_length))
            relative += language_folder_length;
        file.name = relative;
        file.layer = xray::content_patches::FileLayer(full);
        xray::content_patches::FileOwner(full, file.owner);
    }
    std::sort(files.begin(), files.end(), [](const StringTableFile& a, const StringTableFile& b)
    {
        if (a.layer != b.layer)
            return a.layer < b.layer;
        return a.name < b.name;
    });

    xr_parallel_for_each(files, [this](StringTableFile& file)
    {
        Load(file);
    });

    Merge(files);

    if (!translate("st_currency", pData->m_sCurrency) &&
        !translate("ui_st_money_descr", pData->m_sCurrency) && // OGSR
        !translate("ui_st_money_regional", pData->m_sCurrency)) // xp-dev
    {
        pData->m_sCurrency = pSettingsOpenXRay->read_if_exists<pcstr>("gameplay", "currency", "RU");
    }

#ifndef MASTER_GOLD
    Msg("StringTable: loaded %d files", fset.size());
#endif
}

void CStringTable::FillLanguageToken()
{
    ZoneScoped;

    languagesToken.clear();

    string_path path;
    FS.update_path(path, _game_config_, "text" DELIMITER);
    auto languages = FS.file_list_open(path, FS_ListFolders | FS_RootOnly);

    const bool localizationPresent = languages != nullptr;

    // We must warn about lack of localization
    // However we can work without it
    VERIFY(localizationPresent);
    if (localizationPresent)
    {
        int i = 0;
        for (const auto& language : *languages)
        {
            const auto pos = strchr(language, _DELIMITER);
            *pos = '\0'; // we don't need that backslash in the end

            // Skip map_desc folder
            if (0 == xr_strcmp(language, "map_desc"))
                continue;

            bool shouldSkip = false;

            // Open current language folder
            string_path folder;
            strconcat(sizeof(folder), folder, path, language, DELIMITER);
            auto files = FS.file_list_open(folder, FS_ListFiles | FS_RootOnly);

            // Skip empty folder
            if (!files || files->empty())
                shouldSkip = true;

            // Skip folder with only openxray.xml file in it
            // It's important to have 'else if' instead of simple 'if'
            else if (files->size() == 1 && xr_strcmp(files->at(0), OPENXRAY_XML) == 0)
                shouldSkip = true;

            // Don't forget to close opened folder
            FS.file_list_close(files);

            if (shouldSkip)
                continue;

            // Finally, we can add language
            languagesToken.emplace_back(xr_strdup(language), i++); // It's important to have postfix increment!
        }
        FS.file_list_close(languages);
    }

    languagesToken.emplace_back(nullptr, -1);
}

void CStringTable::SetLanguage()
{
    cpcstr defined_language = pSettings->r_string("string_table", "language");
    cpcstr defined_prefix   = pSettings->r_string("string_table", "font_prefix");

    // Before introducing a console command to change the language,
    // mods used to change localization.ltx. We detect that
    // by saving the last value in LanguageIDInLTX
    if (LanguageID != std::numeric_limits<u32>::max() && xr_strcmp(LanguageIDInLTX, defined_language) == 0)
    {
        pData->m_sLanguage = languagesToken.at(LanguageID).name;

        if (0 == xr_strcmp(pData->m_sLanguage, defined_language))
            pData->m_fontPrefix = defined_prefix;
        else
        {
            pData->m_fontPrefix = nullptr;

            constexpr std::tuple<pcstr, pcstr> known_prefixes[] =
            {
                { "fra", "_west" },
                { "ger", "_west" },
                { "ita", "_west" },
                { "spa", "_west" },
                { "pol", "_cent" },
                { "cze", "_cent" },
            };
            for (const auto [language, prefix] : known_prefixes)
            {
                if (0 == xr_strcmp(pData->m_sLanguage, language))
                    pData->m_fontPrefix = prefix;
            }
        }
    }
    else
    {
        pData->m_sLanguage  = defined_language;
        pData->m_fontPrefix = defined_prefix;

        const auto it = std::find_if(languagesToken.begin(), languagesToken.end(), [](const xr_token& token)
        {
            return token.name && token.name == pData->m_sLanguage;
        });

        R_ASSERT3(it != languagesToken.end(), "Check localization.ltx! Current language: ", pData->m_sLanguage.c_str());
        if (it != languagesToken.end())
            LanguageID = it->id;
    }
    xr_strcpy(LanguageIDInLTX, defined_language);
}

shared_str CStringTable::GetCurrentLanguage() const
{
    return pData ? pData->m_sLanguage : nullptr;
}

shared_str CStringTable::GetCurrentFontPrefix() const
{
    return pData ? pData->m_fontPrefix : nullptr;
}

shared_str CStringTable::GetCurrency() const
{
    return pData ? pData->m_sCurrency : "RU";
}

xr_token* CStringTable::GetLanguagesToken() const { return languagesToken.data(); }

void CStringTable::Load(StringTableFile& file) const
{
    ZoneScoped;

    XMLDocument uiXml;
    string_path _s;
    strconcat(sizeof(_s), _s, "text" DELIMITER, pData->m_sLanguage.c_str());

    uiXml.Load(CONFIG_PATH, _s, file.name.c_str());

    //общий список всех записей таблицы в файле
    CONST_XML_NODE root = uiXml.GetRoot();
    const size_t string_num = uiXml.GetNodesNum(root, "string");
    file.strings.reserve(string_num);

    // The <string> nodes one after another, as GetNodesNum counts them (an index into them walks the list from the
    // start each time)
    for (CONST_XML_NODE node = root ? root->FirstChild("string") : nullptr; node; node = node->NextSibling("string"))
    {
        LPCSTR string_name = uiXml.ReadAttrib(node, "id", NULL);
        LPCSTR string_text = uiXml.Read(node->FirstChild("text"), NULL);

        if (!string_text)
        {
#ifndef MASTER_GOLD
            Msg("! [%s] string table entry[%s] doesn't have a translation (no 'text' tag)", file.name.c_str(),
                string_name);
#endif
            continue;
        }
        if (!string_name)
            continue;

        StringTableEntry& entry = file.strings.emplace_back();
        entry.id = string_name;
        entry.value = ParseLine(string_text);
        // A string a patch of this file set (XMLOverride): its owner and layer are the ones of the patch
        if (const pcstr patch_owner = uiXml.ReadAttrib(node, XMLDocument::kPatchOwnerAttr, nullptr))
        {
            entry.owner = patch_owner;
            entry.layer = static_cast<u16>(atoi(uiXml.ReadAttrib(node, XMLDocument::kPatchLayerAttr, "0")));
            entry.over = uiXml.ReadAttrib(node, XMLDocument::kPatchOverAttr, "");
        }
    }
}

void CStringTable::Merge(const xr_vector<StringTableFile>& files)
{
    ZoneScoped;
    using namespace xray::content_patches;

    // The strings in the order of application: by the layer of the string - a string a patch set (XMLOverride) has the
    // layer and the owner of the patch, not of the file patched - then in the order of the files and of a file.
    struct MergeItem
    {
        const StringTableFile* file;
        const StringTableEntry* entry;
        const xr_string* owner;
        u16 layer;
    };
    xr_vector<MergeItem> items;
    for (const StringTableFile& file : files)
    {
        for (const StringTableEntry& entry : file.strings)
        {
            const bool patched = !entry.owner.empty();
            items.push_back({ &file, &entry, patched ? &entry.owner : &file.owner, patched ? entry.layer : file.layer });
        }
    }
    std::stable_sort(items.begin(), items.end(),
        [](const MergeItem& a, const MergeItem& b) { return a.layer < b.layer; });

    // id -> the string that gave the value now: an id of another owner replaces it (the later one wins). An atom
    // (13-extension-model.md 4.2) is recorded for an id two owners give only: the ids of one owner are no conflict.
    xr_string atoms_file = "text" DELIMITER;
    atoms_file += pData->m_sLanguage.c_str();
    xr_map<STRING_ID, u32> origin;
    xr_map<xr_string, u32> replaced_base; // owner -> base ids replaced
    xr_vector<xr_string> previous_owners; // the owners that gave the id before this string
    for (u32 index = 0; index < items.size(); ++index)
    {
        const MergeItem& item = items[index];
        const STRING_ID& id = item.entry->id;
        const xr_string& owner = *item.owner;
        previous_owners.clear();
        // The strings of the id a patch replaced in its file (they never get here) gave it before the patch
        const xr_string& over = item.entry->over;
        for (size_t start = 0; start < over.size();)
        {
            size_t end = over.find(',', start);
            if (end == xr_string::npos)
                end = over.size();
            if (end > start)
                previous_owners.emplace_back(over.substr(start, end - start));
            start = end + 1;
        }
        auto [it, inserted] = origin.emplace(id, index);
        if (!inserted)
        {
            const MergeItem& previous = items[it->second];
#ifndef MASTER_GOLD
            if (*previous.owner == owner && over.empty())
            {
                Msg("~ duplicate string table id [%s] (%s, %s)", id.c_str(), previous.file->name.c_str(),
                    item.file->name.c_str());
            }
#endif
            previous_owners.emplace_back(*previous.owner);
            it->second = index;
        }
        const bool other = std::any_of(previous_owners.begin(), previous_owners.end(),
            [&owner](const xr_string& previous) { return previous != owner; });
        if (other)
        {
            if (std::find(previous_owners.begin(), previous_owners.end(), kBaseOwner) != previous_owners.end())
                ++replaced_base[owner];
            for (const xr_string& previous : previous_owners)
            {
                if (previous != owner)
                    RecordAtom(AtomKind::String, atoms_file.c_str(), id.c_str(), previous.c_str());
            }
            xr_string previous_owner;
            if (RecordAtom(AtomKind::String, atoms_file.c_str(), id.c_str(), owner.c_str(), &previous_owner))
            {
                Msg("~ [addon:%s] string id [%s] (%s%s) is given by addon '%s' too - '%s' wins (the order of the "
                    "addons), console dltx_conflicts", owner.c_str(), id.c_str(), item.file->name.c_str(),
                    item.entry->owner.empty() ? "" : ", patched", previous_owner.c_str(), owner.c_str());
            }
        }
        pData->m_StringTable[id] = item.entry->value;
    }
    for (const auto& [owner, count] : replaced_base)
        Msg("* [addon:%s] replaces %u strings of the base (%s)", owner.c_str(), count, atoms_file.c_str());
}

void CStringTable::ReloadLanguage()
{
    if (0 == xr_strcmp(languagesToken.at(LanguageID).name, pData->m_sLanguage.c_str()))
        return;

    Destroy();
    Init();
}

STRING_VALUE CStringTable::ParseLine(pcstr str)
{
    ZoneScoped;

    constexpr char   ACTION_STR[]     = "$$ACTION_";
    constexpr size_t ACTION_STR_LEN   = std::size(ACTION_STR) - 1;

    constexpr char   ACTION_STR_END[] = "$$";
    constexpr size_t ACTION_STR_END_LEN = std::size(ACTION_STR_END) - 1;

    xr_string string{ str };
    string.erase(std::remove_if(string.begin(), string.end(), [](char ch)
    {
        VERIFY2(ch != GAME_ACTION_MARK,"Using of escape symbol is not allowed in localization.");
        return ch == GAME_ACTION_MARK;
    }), string.end());

    size_t actionStartPos = 0;

    while ((actionStartPos = string.find(ACTION_STR, actionStartPos)) != xr_string::npos)
    {
        const size_t actionEndPos = string.find(ACTION_STR_END, actionStartPos + ACTION_STR_LEN);
        const xr_string actionName = string.substr(actionStartPos + ACTION_STR_LEN, actionEndPos - (actionStartPos + ACTION_STR_LEN));

        if (const auto action = ActionNameToPtr(actionName.c_str())) // if exist, get bindings
        {
            static_assert(kLASTACTION < type_max<u8>, "Modify the code to have more than 255 actions.");
            const char actionCode[] = { GAME_ACTION_MARK, (char)action->id, '\0' };
            string.replace(actionStartPos, actionEndPos + ACTION_STR_END_LEN - actionStartPos, actionCode);
            actionStartPos += std::size(actionCode) - 1;
        }
        else // skip
        {
            actionStartPos = actionEndPos + ACTION_STR_END_LEN;
        }
    }

    return { string.c_str() };
}

void CStringTable::replace(const STRING_ID& str_id, const STRING_VALUE& new_str)
{
    if (!pData)
        return;

    if (pData->m_StringTable.find(str_id) != pData->m_StringTable.end())
        pData->m_StringTable[str_id] = new_str;
}

void CStringTable::register_token(const STRING_ID& key, const STRING_VALUE& value)
{
    if (!pData)
        return;

    std::lock_guard guard{ pDataMutex };
    pData->m_tokens[key] = value;
}

void CStringTable::unregister_token(const STRING_ID& key)
{
    if (!pData)
        return;

    std::lock_guard guard{ pDataMutex };
    pData->m_tokens.erase(key);
}

STRING_VALUE CStringTable::apply_tokens(const STRING_VALUE& str) const
{
    if (!pData || pData->m_tokens.empty())
        return str;

    const char* cstr = str.c_str();
    if (!strchr(cstr, '@'))
        return str;

    xr_string result;
    result.reserve(str.size());
    const char* p = cstr;

    while (*p)
    {
        if (*p != '@')
        {
            result += *p++;
            continue;
        }

        const char* end = strchr(p + 1, '@');
        if (!end || end == p + 1) // no closing '@' or empty key '@@'
        {
            result += *p++;
            continue;
        }

        const xr_string key(p + 1, end);
        const STRING_ID key_id(key.c_str());

        // Priority 1: token map
        const auto tok_it = pData->m_tokens.find(key_id);
        if (tok_it != pData->m_tokens.end())
        {
            result += tok_it->second.c_str();
            p = end + 1;
            continue;
        }

        // Priority 2: StringTable ID substitution (no recursion - raw value only)
        const auto str_it = pData->m_StringTable.find(key_id);
        if (str_it != pData->m_StringTable.end())
        {
            result += str_it->second.c_str();
            p = end + 1;
            continue;
        }

        // Fallback: keep token as-is
        result += '@';
        result += key;
        result += '@';
        p = end + 1;
    }

    return STRING_VALUE(result.c_str());
}

STRING_VALUE CStringTable::translate(const STRING_ID& str_id) const
{
    if (!pData)
        return str_id;

    const auto it = pData->m_StringTable.find(str_id);
    if (it != pData->m_StringTable.end())
        return apply_tokens(it->second);
    return str_id;
}

STRING_VALUE CStringTable::translate(const STRING_ID& str_id, const STRING_ID& str_id2) const
{
    STRING_VALUE out = str_id;
    if (!translate(str_id, out))
        translate(str_id2, out);
    return out;
}

bool CStringTable::translate(const STRING_ID& str_id, STRING_VALUE& out) const
{
    if (!pData)
        return false;

    const auto it = pData->m_StringTable.find(str_id);
    if (it != pData->m_StringTable.end())
    {
        out = apply_tokens(it->second);
        return true;
    }
    return false;
}

bool CStringTable::has_translation(const STRING_ID& str_id) const
{
    if (!pData)
        return false;

    return pData->m_StringTable.find(str_id) != pData->m_StringTable.end();
}

bool CStringTable::exist(const STRING_ID& str_id) const
{
    if (!pData)
        return false;

    if (pData->m_StringTable.find(str_id) != pData->m_StringTable.end())
        return true;

    return false;
}
