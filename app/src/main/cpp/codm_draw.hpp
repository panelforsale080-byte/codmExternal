#pragma once
#include "codm_engine.hpp"

struct ImDrawList;

void codm_draw_esp(ImDrawList *dl, float screenW, float screenH, const CodmFrame &fr,
                   const CodmFeatures &feat);
