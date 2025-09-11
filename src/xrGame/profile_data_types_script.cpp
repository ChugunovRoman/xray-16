#include "StdAfx.h"

#include "profile_data_types.h"
#include "profile_data_types_script.h"

namespace gamespy_profile
{
void profile_data_script_registrator::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<award_data>("award_data")
            .def_readonly("m_count", &award_data::m_count)
            .def_readonly("m_last_reward_date", &award_data::m_last_reward_date),

        class_<all_awards_t::value_type>("award_pair_t")
            .def_readonly("first", &all_awards_t::value_type::first)
            .def_readonly("second", &all_awards_t::value_type::second),

        class_<all_best_scores_t::value_type>("best_scores_pair_t")
            .def_readonly("first", &all_best_scores_t::value_type::first)
            .def_readonly("second", &all_best_scores_t::value_type::second)
    ];
}
} // namespace gamespy_profile
DEFINE_MIXED_DELEGATE_SCRIPT(gamespy_profile::store_operation_cb, "store_operation_cb");
