#include "ui_core.hpp"
#include "codm_draw.hpp"
#include "codm_engine.hpp"
#include "codm_anogs.hpp"
#include "codm_proc.hpp"
#include "clip_android.hpp"
#include "overlay_jni.hpp"
#include "ui_log.hpp"

#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"

#include <GLES2/gl2.h>
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

ImVec2 initialScreenSize{0.f, 0.f};

static int g_w = 0, g_h = 0;
static int g_displayW = 0, g_displayH = 0;
static bool g_glReady = false;
static bool g_imgui = false;
static std::mutex g_mu;
static const char *g_glsl = "#version 100";

static bool g_showMenu = false;
static bool g_espOn = false;
static CodmFeatures g_feat = [] {
    CodmFeatures f{};
    f.box = f.line = f.health = f.distance = f.name = f.armor = f.counter = true;
    return f;
}();
static CodmFrame g_frame{};

static std::atomic<bool> g_runReader{false};
static std::thread g_reader;

static const char *status_name(CodmStatus s) {
    switch (s) {
    case CodmStatus::Waiting: return "Waiting";
    case CodmStatus::NoProcess: return "NoProcess";
    case CodmStatus::NoUnity: return "NoUnity";
    case CodmStatus::NoMatch: return "NoMatch";
    case CodmStatus::Ready: return "Ready";
    }
    return "?";
}

static void reader_loop() {
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    while (g_runReader.load()) {
        CodmFeatures feat = g_feat;
        if (g_espOn) feat.esp = true;
        else feat.esp = false;
        CodmFrame tick = codm_tick(feat);
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_frame = std::move(tick);
        }
        next += std::chrono::milliseconds(g_espOn || g_showMenu ? 33 : 200);
        std::this_thread::sleep_until(next);
    }
}

static void apply_style() {
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 14.f;
    s.FrameRounding = 10.f;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.15f, 0.15f, 0.17f, 0.94f);
    c[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.12f, 0.14f, 1.f);
    c[ImGuiCol_Button] = ImVec4(0.35f, 0.35f, 0.38f, 1.f);
}

static void draw_ui() {
    float sw = (float)g_w;
    float sh = (float)g_h;
    if (sw < 1.f && g_displayW > 0) sw = (float)g_displayW;
    if (sh < 1.f && g_displayH > 0) sh = (float)g_displayH;
    /* Caller (ui_gl_draw_frame) already holds g_mu — do not lock again. */
    CodmFrame fr = g_frame;

    if (g_espOn && fr.status == CodmStatus::Ready) {
        ImDrawList *bg = ImGui::GetBackgroundDrawList();
        codm_draw_esp(bg, sw, sh, fr, g_feat);
    }

    if (g_showMenu) {
        ImDrawList *bg = ImGui::GetBackgroundDrawList();
        bg->AddRectFilled(ImVec2(0, 0), ImVec2(sw, sh), IM_COL32(0, 0, 0, 140));
    }

    if (g_showMenu) {
    const float mw = sw > 420.f ? 400.f : sw * 0.9f;
    const float mh = sh > 520.f ? 480.f : sh * 0.75f;
    ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(24.f, 48.f), ImGuiCond_Always);
    if (ImGui::Begin("CodmExternal", &g_showMenu,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextDisabled("Target: com.garena.game.codm");
        ImGui::Text("Status: %s", status_name(fr.status));
        if (fr.hint[0]) ImGui::TextWrapped("%s", fr.hint);
        if (fr.pid) ImGui::Text("pid=%d unity=0x%llx anogs=%s", (int)fr.pid,
                                (unsigned long long)fr.unity,
                                fr.anogsPatched ? "patched" : (fr.anogs ? "ready" : "—"));

        if (ImGui::Button("Find game", ImVec2(-1, 0))) {
            const pid_t p = proc_best_codm();
            codm_set_want_pid(p);
            codm_anogs_force_repatch();
            ui_log("[scan] pick pid=%d", (int)p);
        }
        if (ImGui::Button("Patch ANOGS (hooks.h)", ImVec2(-1, 0))) {
            codm_anogs_force_repatch();
            ui_log("[anogs] manual re-patch requested");
        }

        ImGui::Separator();
        if (ImGui::Checkbox("ESP ENABLE", &g_espOn)) {
            if (g_espOn) ui_esp_start();
            else ui_esp_stop();
        }
        ImGui::BeginChild("feats", ImVec2(0, 210), true);
        ImGui::Checkbox("ESP BOX", &g_feat.box);
        ImGui::Checkbox("3D BOX", &g_feat.box3d);
        ImGui::Checkbox("ESP LINE", &g_feat.line);
        ImGui::Checkbox("ESP HEALTH", &g_feat.health);
        ImGui::Checkbox("ESP ARMOR", &g_feat.armor);
        ImGui::Checkbox("ESP DISTANCE", &g_feat.distance);
        ImGui::Checkbox("ESP NAME", &g_feat.name);
        ImGui::Checkbox("ESP PLAYER ID", &g_feat.playerId);
        ImGui::Checkbox("ESP CIRCLE", &g_feat.circle);
        ImGui::Checkbox("ESP ALERT 360", &g_feat.alert);
        ImGui::Checkbox("ENEMY COUNTER", &g_feat.counter);
        ImGui::Checkbox("IGNORE KNOCKED", &g_feat.ignoreKnocked);
        ImGui::Checkbox("ESP VEHICLE", &g_feat.vehicle);
        ImGui::Combo("BOX STYLE", &g_feat.boxStyle, "Corners\0Full\0");
        ImGui::Combo("LINE POS", &g_feat.linePos, "Bottom\0Top\0");
        ImGui::EndChild();
        if (g_espOn) g_feat.esp = true;

        ImGui::Separator();
        if (ImGui::Button("Copy logs", ImVec2(-1, 0))) {
            static char logCopy[16384];
            ui_log_copy_all(logCopy, sizeof(logCopy));
            if (android_copy_to_clipboard(logCopy)) {
                ui_log("[ui] logs copied");
                jni_toast("Logs copied");
            } else {
                ui_log("[ui] copy logs failed");
                jni_toast("Copy logs failed");
            }
        }
        static char logBuf[8192];
        ui_log_copy_all(logBuf, sizeof(logBuf));
        ImGui::BeginChild("logs", ImVec2(0, 140), true);
        ImGui::TextUnformatted(logBuf);
        ImGui::SetScrollHereY(1.f);
        ImGui::EndChild();

        ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f), "Tap X — close menu");
    }
    ImGui::End();
    }

    ui_sync_menu_touch_state();
}

static void render_frame() {
    const int fw = g_w > 0 ? g_w : g_displayW;
    const int fh = g_h > 0 ? g_h : g_displayH;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(fw > 0 ? fw : 1, fh > 0 ? fh : 1);
    ImGui::NewFrame();
    draw_ui();
    ImGui::Render();
    const int vw = fw > 0 ? fw : 1;
    const int vh = fh > 0 ? fh : 1;
    glViewport(0, 0, vw, vh);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void ui_gl_surface_created() {}

void ui_gl_surface_changed(int w, int h) {
    g_w = w;
    g_h = h;
    if (initialScreenSize.x <= 0.f && w > 0)
        initialScreenSize = ImVec2((float)w, (float)h);
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_glReady) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
        apply_style();
        ImGui_ImplAndroid_Init(nullptr);
        ImGui_ImplOpenGL3_Init(g_glsl);
        g_imgui = true;
        g_glReady = true;
        g_runReader.store(true);
        g_reader = std::thread(reader_loop);
    }
}

void ui_gl_surface_destroyed() {
    g_runReader.store(false);
    if (g_reader.joinable()) g_reader.join();
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_imgui) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplAndroid_Shutdown();
        ImGui::DestroyContext();
        g_imgui = false;
    }
    g_glReady = false;
}

void ui_gl_draw_frame() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_glReady) return;
    render_frame();
}

int ui_gl_frame_delay_ms() {
    if (g_showMenu || g_espOn) return 16;
    return 200;
}

void ui_touch(int action, float x, float y) {
    if (!g_glReady) return;
    ImGuiIO &io = ImGui::GetIO();
    io.MousePos = ImVec2(x, y);
    const int a = action & 0xff;
    switch (a) {
    case 0: /* ACTION_DOWN */
    case 5: /* ACTION_POINTER_DOWN */
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, true);
        break;
    case 1: /* ACTION_UP */
    case 6: /* ACTION_POINTER_UP */
    case 3: /* ACTION_CANCEL */
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, false);
        break;
    case 2: /* ACTION_MOVE */
        io.AddMousePosEvent(x, y);
        break;
    default:
        break;
    }
}

void ui_set_display_size(int w, int h) {
    if (w > 0) g_displayW = w;
    if (h > 0) g_displayH = h;
}

void ui_open_menu() {
    g_showMenu = true;
    if (g_w <= 0 && g_displayW > 0) {
        g_w = g_displayW;
        g_h = g_displayH;
    }
}

void ui_toggle_menu() { g_showMenu = !g_showMenu; }

void ui_minimize_menu() {
    g_showMenu = false;
    ui_sync_menu_touch_state();
}
bool ui_menu_interactive() { return g_showMenu; }
bool ui_needs_continuous_gl() { return g_showMenu || g_espOn; }

void ui_esp_start() {
    g_espOn = true;
    g_feat.esp = true;
}
void ui_esp_stop() {
    g_espOn = false;
    g_feat.esp = false;
}

static void *g_ctx = nullptr;
void ui_set_context(void *ctx) { g_ctx = ctx; }

static bool g_menuTouch = false;

void ui_sync_menu_touch_state() {
    const bool on = g_showMenu;
    if (on == g_menuTouch) return;
    g_menuTouch = on;
    jni_set_menu_touch(on);
}
