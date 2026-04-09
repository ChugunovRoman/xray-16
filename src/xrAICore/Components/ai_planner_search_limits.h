////////////////////////////////////////////////////////////////////////////
// Tunable max node count for nested GOAP solves (CProblemSolver::solve).
// Upper brain graph search is capped separately in stalker_planner.cpp.
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "xrAICore/AISpaceBase.hpp"

extern XRAICORE_API u32 g_ai_nested_planner_graph_search_max_nodes;
