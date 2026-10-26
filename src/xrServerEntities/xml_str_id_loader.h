#pragma once

#include "Common/object_broker.h"

#include "xrUICore/XML/xrUIXmlParser.h"

// T_INIT -  класс где определена статическая InitXmlIdToIndex
//          функция инициализации file_str и tag_name

//структура хранит строковый id элемента
//файл и позицию, где этот элемент находится
struct ITEM_DATA
{
    shared_str id;
    int index;
    int pos_in_file;
    CUIXml* _xml;
    // plans/lua_to_cpp/57-gw-eoc-next-steps.md (E1): the node NavigateToNode(tag_name, pos_in_file) from the root of
    // _xml finds - found once in InitInternal (the DOM is not changed after the load); NavigateToNode walks the
    // siblings from the first one (~12.7M steps for all ~5.5k specific characters, most in one file)
    XML_NODE node;
};
typedef xr_vector<ITEM_DATA> T_VECTOR;

void _destroy_item_data_vector_cont(T_VECTOR* vec);

#define TEMPLATE_SPECIALIZATION template <typename T_INIT>
#define CSXML_IdToIndex CXML_IdToIndex<T_INIT>

TEMPLATE_SPECIALIZATION
class CXML_IdToIndex
{
private:
    static T_VECTOR* m_pItemDataVector;
    // plans/lua_to_cpp/57-gw-eoc-next-steps.md (E1, 53 section 5): id -> the position in m_pItemDataVector. GetById was
    // a linear search over all the items (~5.5k specific characters: the index of 53 loads every profile once per
    // process - ~15M comparisons, spec_char/index_build ~187 ms). The ids are unique (InitInternal skips a duplicate,
    // the first one wins), the vector is filled only there, so the hash finds the item the linear search found
    using T_ID_MAP = xr_unordered_map<shared_str, u32>;
    static T_ID_MAP* m_pIdMap;

protected:
    CXML_IdToIndex() = default;

    //имена xml файлов (разделенных запятой) из которых
    //производить загрузку элементов
    static LPCSTR file_str;
    //имена тегов
    static LPCSTR tag_name;

public:
    static void InitInternal(bool crashOnFail = true, bool ignoreMissingEndTagError = false);

    static const ITEM_DATA* GetById(const shared_str& str_id, bool no_assert = false);
    static const ITEM_DATA* GetByIndex(int index, bool no_assert = false);

    static int IdToIndex(const shared_str& str_id, int default_index = -1, bool no_assert = false)
    {
        const ITEM_DATA* item = GetById(str_id, no_assert);
        return item ? item->index : default_index;
    }
    static shared_str IndexToId(int index, shared_str default_id = nullptr, bool no_assert = false)
    {
        const ITEM_DATA* item = GetByIndex(index, no_assert);
        return item ? item->id : default_id;
    }

    static int GetMaxIndex() { return m_pItemDataVector->size() - 1; }
    //удаление статичекого массива
    static void DeleteIdToIndexData();
};

TEMPLATE_SPECIALIZATION
T_VECTOR* CSXML_IdToIndex::m_pItemDataVector = nullptr;
TEMPLATE_SPECIALIZATION
typename CSXML_IdToIndex::T_ID_MAP* CSXML_IdToIndex::m_pIdMap = nullptr;

TEMPLATE_SPECIALIZATION
LPCSTR CSXML_IdToIndex::file_str = nullptr;
TEMPLATE_SPECIALIZATION
LPCSTR CSXML_IdToIndex::tag_name = nullptr;

TEMPLATE_SPECIALIZATION
const ITEM_DATA* CSXML_IdToIndex::GetById(const shared_str& str_id, bool no_assert)
{
    T_INIT::InitXmlIdToIndex();
    const auto found = m_pIdMap->find(str_id);
    if (found == m_pIdMap->end())
    {
        R_ASSERT3(no_assert, "item not found, id", str_id.c_str());
        return nullptr;
    }

    return &(*m_pItemDataVector)[found->second];
}

TEMPLATE_SPECIALIZATION
const ITEM_DATA* CSXML_IdToIndex::GetByIndex(int index, bool no_assert)
{
    if ((size_t)index >= m_pItemDataVector->size())
    {
        R_ASSERT3(no_assert, "item by index not found in files", file_str);
        return nullptr;
    }
    return &(*m_pItemDataVector)[index];
}

TEMPLATE_SPECIALIZATION
void CSXML_IdToIndex::DeleteIdToIndexData()
{
    VERIFY(m_pItemDataVector);
    _destroy_item_data_vector_cont(m_pItemDataVector);

    xr_delete(m_pItemDataVector);
    xr_delete(m_pIdMap);
}

TEMPLATE_SPECIALIZATION
void CSXML_IdToIndex::InitInternal(bool crashOnFail /*= true*/, bool ignoreMissingEndTagError /*= false*/)
{
    VERIFY(!m_pItemDataVector);
    T_INIT::InitXmlIdToIndex();

    m_pItemDataVector = xr_new<T_VECTOR>();
    m_pIdMap = xr_new<T_ID_MAP>();

    Msg("file_str: %s", file_str);
    VERIFY(file_str);
    R_ASSERT2_CURE(tag_name, "XML tag name is missing", { return; });

    string_path xml_file;
    int count = _GetItemCount(file_str);
    int index = 0;
    for (int it = 0; it < count; ++it)
    {
        _GetItem(file_str, it, xml_file);

        CUIXml* uiXml = xr_new<CUIXml>();
        xr_string xml_file_full;
        xml_file_full = xml_file;
        xml_file_full += ".xml";

        uiXml->IgnoreMissingEndTagError(ignoreMissingEndTagError);
        const bool success = uiXml->Load(CONFIG_PATH, "gameplay", xml_file_full.c_str(), crashOnFail);
        if (!success)
        {
            delete_data(uiXml);
            continue;
        }

        //общий список
        int items_num = uiXml->GetNodesNum(uiXml->GetRoot(), tag_name);

        // item_node - the i-th node of tag_name, as NavigateToNode(root, tag_name, i) finds it (FirstChild, then i
        // times NextSibling of the tag), walked once instead of from the first one for every i
        XML_NODE item_node = items_num > 0 ? uiXml->GetRoot()->FirstChild(tag_name) : nullptr;
        for (int i = 0; i < items_num; ++i, item_node = item_node ? item_node->NextSibling(tag_name) : nullptr)
        {
            LPCSTR item_name = uiXml->ReadAttrib(item_node, "id", nullptr);
            if (!item_name || !item_name[0])
            {
#ifndef MASTER_GOLD
                Msg("! id for %s don't set, number %d in %s", tag_name, i, xml_file_full.c_str());
#endif
                continue;
            }

            //проверить ID на уникальность (the hash of the ids instead of the linear search, the first one wins as
            //before)
            const shared_str item_id(item_name);
            if (!m_pIdMap->emplace(item_id, static_cast<u32>(m_pItemDataVector->size())).second)
            {
#ifndef MASTER_GOLD
                Msg("! duplicate %s with id[%s] in %s", tag_name, item_name, xml_file_full.c_str());
#endif
                continue;
            }

            ITEM_DATA data;
            data.id = item_id;
            data.index = index;
            data.pos_in_file = i;
            //.				data.file_name		= xml_file;
            data._xml = uiXml;
            data.node = item_node;
            m_pItemDataVector->push_back(data);

            index++;
        }
        if (0 == items_num)
            delete_data(uiXml);
    }
}

#undef TEMPLATE_SPECIALIZATION
