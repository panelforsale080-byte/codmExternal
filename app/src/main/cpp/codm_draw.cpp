#include "codm_draw.hpp"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

static void corner_box(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col) {
    const float cw = (b.x - a.x) * 0.25f;
    const float ch = (b.y - a.y) * 0.25f;
    dl->AddLine(a, ImVec2(a.x + cw, a.y), col, 1.f);
    dl->AddLine(a, ImVec2(a.x, a.y + ch), col, 1.f);
    dl->AddLine(ImVec2(b.x, a.y), ImVec2(b.x - cw, a.y), col, 1.f);
    dl->AddLine(ImVec2(b.x, a.y), ImVec2(b.x, a.y + ch), col, 1.f);
    dl->AddLine(ImVec2(a.x, b.y), ImVec2(a.x + cw, b.y), col, 1.f);
    dl->AddLine(ImVec2(a.x, b.y), ImVec2(a.x, b.y - ch), col, 1.f);
    dl->AddLine(b, ImVec2(b.x - cw, b.y), col, 1.f);
    dl->AddLine(b, ImVec2(b.x, b.y - ch), col, 1.f);
}

void codm_draw_esp(ImDrawList *dl, float screenW, float screenH, const CodmFrame &fr,
                   const CodmFeatures &feat) {
    if (!dl || !feat.esp || fr.status != CodmStatus::Ready) return;

    if (feat.counter) {
        char label[64];
        std::snprintf(label, sizeof(label), "[Player]%d   [AI]%d", fr.players, fr.bots);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2((screenW - ts.x) * 0.5f, 48.f), IM_COL32(255, 255, 255, 255), label);
    }

    for (const CodmEnemy &e : fr.enemies) {
        if (!e.alive || !e.screenOk) continue;
        const float hx = e.headScreen.x;
        const float hy = screenH - e.headScreen.y;
        const float rx = e.screen.x;
        const float ry = screenH - e.screen.y;
        if (e.vehicle) {
            if (!feat.vehicle) continue;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.0f FT", e.dist);
            dl->AddText(ImVec2(rx, ry), IM_COL32(255, 255, 255, 255), buf);
            continue;
        }
        float boxH = std::fabs(e.headScreen.y - e.screen.y) * 1.1f;
        float boxW = boxH * 0.40f;
        if (boxH < 8.f) {
            boxH = 72.f;
            boxW = 38.f;
        }
        const ImVec2 a(hx - boxW * 0.5f, hy - boxH * 0.1f);
        const ImVec2 b(a.x + boxW, a.y + boxH);
        const bool onScreen = !(b.x < 0 || a.x > screenW || b.y < 0 || a.y > screenH);
        const ImU32 col = e.knocked ? IM_COL32(255, 180, 40, 230)
                                    : (e.bot ? IM_COL32(80, 220, 255, 230) : IM_COL32(255, 60, 60, 230));

        if (feat.alert) {
            const float m = 28.f;
            const bool edge = hx < screenW * 0.08f || hx > screenW * 0.92f || hy < screenH * 0.08f ||
                              hy > screenH * 0.92f;
            if (edge) {
                const float dx = std::clamp(hx, m, screenW - m);
                const float dy = std::clamp(hy, m, screenH - m);
                dl->AddCircleFilled(ImVec2(dx, dy), 10.f, IM_COL32(255, 255, 255, 255));
            }
        }
        if (!onScreen) continue;

        if (feat.circle)
            dl->AddCircle(ImVec2(rx, ry), boxW * 0.55f, col, 24, 1.5f);
        if (feat.box) {
            if (feat.boxStyle == 1) dl->AddRect(a, b, col, 2.f, 0, 1.f);
            else corner_box(dl, a, b, col);
        }
        if (feat.box3d) {
            const float w = boxW * 0.35f;
            dl->AddRect(ImVec2(a.x + w, a.y - w), ImVec2(b.x + w, b.y - w), col, 0.f, 0, 1.f);
            dl->AddLine(a, ImVec2(a.x + w, a.y - w), col, 1.f);
            dl->AddLine(ImVec2(b.x, a.y), ImVec2(b.x + w, a.y - w), col, 1.f);
            dl->AddLine(ImVec2(a.x, b.y), ImVec2(a.x + w, b.y - w), col, 1.f);
            dl->AddLine(b, ImVec2(b.x + w, b.y - w), col, 1.f);
        }
        if (feat.line) {
            const ImVec2 from = feat.linePos == 1 ? ImVec2(screenW * 0.5f, 20.f)
                                                  : ImVec2(screenW * 0.5f, screenH);
            const ImVec2 to = feat.linePos == 1 ? ImVec2(hx, hy) : ImVec2(rx, ry);
            dl->AddLine(from, to, col, 1.5f);
        }
        if (feat.health && e.hpMax > 1.f) {
            const float t = std::clamp(e.hp / e.hpMax, 0.f, 1.f);
            const ImU32 hc = t > 0.5f ? IM_COL32(0, 255, 0, 255)
                                      : (t > 0.2f ? IM_COL32(255, 165, 0, 255) : IM_COL32(255, 0, 0, 255));
            dl->AddRectFilled(ImVec2(a.x - 6.f, a.y), ImVec2(a.x - 2.f, b.y), IM_COL32(40, 40, 40, 200));
            const float top = a.y + (b.y - a.y) * (1.f - t);
            dl->AddRectFilled(ImVec2(a.x - 6.f, top), ImVec2(a.x - 2.f, b.y), hc);
        } else if (feat.armor && e.armorMax > 0) {
            const float t = std::clamp(static_cast<float>(e.armor) / static_cast<float>(e.armorMax), 0.f, 1.f);
            dl->AddRectFilled(ImVec2(a.x - 12.f, a.y), ImVec2(a.x - 8.f, b.y), IM_COL32(40, 40, 40, 200));
            const float top = a.y + (b.y - a.y) * (1.f - t);
            dl->AddRectFilled(ImVec2(a.x - 12.f, top), ImVec2(a.x - 8.f, b.y), IM_COL32(0, 191, 255, 255));
        }
        if (feat.armor && e.armorMax > 0 && e.hpMax > 1.f) {
            const float t = std::clamp(static_cast<float>(e.armor) / static_cast<float>(e.armorMax), 0.f, 1.f);
            dl->AddRectFilled(ImVec2(a.x - 12.f, a.y), ImVec2(a.x - 8.f, b.y), IM_COL32(40, 40, 40, 200));
            const float top = a.y + (b.y - a.y) * (1.f - t);
            dl->AddRectFilled(ImVec2(a.x - 12.f, top), ImVec2(a.x - 8.f, b.y), IM_COL32(0, 191, 255, 255));
        }
        if (feat.name) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%s%s", e.bot ? "[BOT] " : "[PLAYER] ",
                          e.name[0] ? e.name : "UNKNOWN");
            const ImVec2 ts = ImGui::CalcTextSize(buf);
            dl->AddText(ImVec2(a.x + (b.x - a.x - ts.x) * 0.5f, a.y - ts.y - 2.f),
                        IM_COL32(255, 255, 255, 255), buf);
        }
        if (feat.playerId && e.team >= 0) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "ID: %d", e.team);
            dl->AddText(ImVec2(hx - 32.f, hy - 16.f), IM_COL32(255, 255, 255, 255), buf);
        }
        if (feat.distance) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.0f FT", e.dist);
            dl->AddText(ImVec2(hx + 18.f, hy), IM_COL32(255, 255, 255, 230), buf);
        }
    }
}
