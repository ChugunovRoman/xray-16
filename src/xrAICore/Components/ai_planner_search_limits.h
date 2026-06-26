////////////////////////////////////////////////////////////////////////////
// Tunable max node count for nested GOAP solves (CProblemSolver::solve).
// Upper brain graph search is capped separately in stalker_planner.cpp.
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "xrAICore/AISpaceBase.hpp"

extern XRAICORE_API u32 g_ai_nested_planner_graph_search_max_nodes;

// Per-solve evaluator cache epoch. Bumped at the start of each GOAP solve window
// (CProblemSolver::solve / CStalkerPlanner::update). Script property evaluators cache their
// result for one epoch so the world is read once per solve instead of once per A* node.
extern XRAICORE_API u32 g_ai_evaluator_solve_epoch;
