#include "codm_draw.hpp"
#include "imgui.h"
#include <cmath>
#include <cstdio>

void codm_draw_esp(ImDrawList *dl, float screenW, float screenH, const CodmFrame &fr,
                   const CodmFeatures &feat) {
    if (!dl || !feat.esp || fr.status != CodmStatus::Ready) return;

    for (const CodmEnemy &e : fr.enemies) {
        if (!e.alive || !e.screenOk) continue;
        const float hx = e.headScreen.x;
        const float hy = screenH - e.headScreen.y;
        const float rx = e.screen.x;
        const float ry = screenH - e.screen.y;
        float boxH = std::fabs(e.headScreen.y - e.screen.y) * 1.1f;
        float boxW = boxH * 0.40f;
        if (boxH < 8.f) {
            boxH = 72.f;
            boxW = 38.f;
        }
        const ImVec2 a(hx - boxW * 0.5f, hy - boxH * 0.1f);
        const ImVec2 b(a.x + boxW, a.y + boxH);
        if (b.x < 0 || a.x > screenW || b.y < 0 || a.y > screenH) continue;
        const float sx = rx;
        const float sy = ry;

        if (feat.box)
            dl->AddRect(a, b, IM_COL32(255, 60, 60, 230), 0.f, 0, 2.f);
        if (feat.line)
            dl->AddLine(ImVec2(screenW * 0.5f, screenH), ImVec2(sx, sy), IM_COL32(255, 220, 80, 200), 1.5f);
        if (feat.health && e.hpMax > 1.f) {
            const float t = std::clamp(e.hp / e.hpMax, 0.f, 1.f);
            dl->AddRectFilled(ImVec2(a.x - 5.f, b.y), ImVec2(a.x - 2.f, a.y), IM_COL32(40, 40, 40, 200));
            const float top = b.y - (b.y - a.y) * t;
            dl->AddRectFilled(ImVec2(a.x - 5.f, top), ImVec2(a.x - 2.f, b.y), IM_COL32(80, 220, 100, 240));
        }
        if (feat.distance) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.0fm", e.dist);
            dl->AddText(ImVec2(a.x, b.y + 2.f), IM_COL32(255, 255, 255, 230), buf);
        }
    }
}
