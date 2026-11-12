#include "ui_core.hpp"
#include "codm_draw.hpp"
#include "codm_engine.hpp"
#include "codm_proc.hpp"
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

static int g_w = 0, g_h = 0;
static bool g_glReady = false;
static bool g_imgui = false;
static std::mutex g_mu;
static const char *g_glsl = "#version 100";

static bool g_showMenu = false;
static bool g_espOn = false;
static CodmFeatures g_feat{};
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
    const float sw = (float)g_w;
    const float sh = (float)g_h;
    CodmFrame fr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        fr = g_frame;
    }

    if (g_espOn && fr.status == CodmStatus::Ready) {
        ImDrawList *bg = ImGui::GetBackgroundDrawList();
        codm_draw_esp(bg, sw, sh, fr, g_feat);
    }

    if (!g_showMenu) return;

    ImGui::SetNextWindowSize(ImVec2(380, 460), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(24, 48), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("CodmExternal", &g_showMenu, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextDisabled("Target: com.garena.game.codm");
        ImGui::Text("Status: %s", status_name(fr.status));
        if (fr.hint[0]) ImGui::TextWrapped("%s", fr.hint);
        if (fr.pid) ImGui::Text("pid=%d unity=0x%llx anogs=%s", (int)fr.pid,
                                (unsigned long long)fr.unity,
                                fr.anogsPatched ? "patched" : (fr.anogs ? "ready" : "—"));

        if (ImGui::Button("Find game", ImVec2(-1, 0))) {
            const pid_t p = proc_best_codm();
            codm_set_want_pid(p);
            ui_log("[scan] pick pid=%d", (int)p);
        }

        ImGui::Separator();
        if (ImGui::Checkbox("ESP", &g_espOn)) {
            if (g_espOn) ui_esp_start();
            else ui_esp_stop();
        }
        ImGui::Checkbox("Box", &g_feat.box);
        ImGui::Checkbox("Line", &g_feat.line);
        ImGui::Checkbox("Health", &g_feat.health);
        ImGui::Checkbox("Distance", &g_feat.distance);
        if (g_espOn) g_feat.esp = true;

        ImGui::Separator();
        static char logBuf[8192];
        ui_log_copy_all(logBuf, sizeof(logBuf));
        ImGui::BeginChild("logs", ImVec2(0, 140), true);
        ImGui::TextUnformatted(logBuf);
        ImGui::SetScrollHereY(1.f);
        ImGui::EndChild();

        ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f), "Tap X — close menu");
    }
    ImGui::End();
    ui_sync_menu_touch_state();
}

static void render_frame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_w, g_h);
    ImGui::NewFrame();
    draw_ui();
    ImGui::Render();
    glViewport(0, 0, g_w, g_h);
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
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_glReady) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
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
    if (action == 0) io.MouseDown[0] = true;
    else if (action == 1) io.MouseDown[0] = false;
}

void ui_open_menu() { g_showMenu = true; }
void ui_toggle_menu() { g_showMenu = !g_showMenu; }
void ui_minimize_menu() { g_showMenu = false; }
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
