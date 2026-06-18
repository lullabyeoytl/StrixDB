#pragma once

#include "common/context.h"
#include "optimizer/plan.h"
#include "system/sm.h"

void execute_load_plan(const LoadPlan &plan, SmManager *sm_manager, Context *context);
