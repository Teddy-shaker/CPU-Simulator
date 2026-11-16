// Window, OpenGL and Dear ImGui setup. All drawing lives in ui.cpp.
//
// Command line (mostly for automated checks):
//   --sample N          load built-in sample N at startup
//   --steps N           clock the pipeline N cycles at startup
//   --hazard 0|1|2      none / stall / forwarding
//   --penalty N         cache miss penalty in cycles
//   --inspect N         open the explanation for stage N (0 = IF ... 4 = WB)
//   --lesson N --next K start lesson N and press Continue K times
//   --challenge 1       turn the challenge on and run to the first question
//   --answer N          answer that question with option N
//   --theme N           0 midnight blue, 1 black, 2 light
//   --scale F           text and interface size (1.0 = 100%)
//   --window NAME       open "settings" or "about"
//   --screenshot FILE   render, save the frame as a binary PPM and exit
//                       (settings are neither loaded nor saved in this mode)
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "samples.h"
#include "ui.h"

static bool file_exists(const char* path) {
    if (FILE* f = std::fopen(path, "rb")) {
        std::fclose(f);
        return true;
    }
    return false;
}

static ImFont* load_font(const char* path, float size) {
    ImGuiIO& io = ImGui::GetIO();
    if (file_exists(path)) return io.Fonts->AddFontFromFileTTF(path, size);
    ImFontConfig cfg;
    cfg.SizePixels = size;
    return io.Fonts->AddFontDefault(&cfg);
}

// (Re)creates the fonts and the style for the current scale and theme.
static void restyle(App& app, bool first) {
    ImGuiIO& io = ImGui::GetIO();
    app.scale = app.dpi_scale * app.settings.ui_scale;
    if (!first) ImGui_ImplOpenGL3_DestroyFontsTexture();
    io.Fonts->Clear();
    app.font_ui = load_font("C:\\Windows\\Fonts\\segoeui.ttf", std::floor(16.0f * app.scale));
    app.font_mono = load_font("C:\\Windows\\Fonts\\consola.ttf", std::floor(14.0f * app.scale));
    app.font_title = load_font("C:\\Windows\\Fonts\\segoeuib.ttf", std::floor(18.0f * app.scale));
    if (!first) ImGui_ImplOpenGL3_CreateFontsTexture();
    app.apply_style();
    app.restyle = false;
}

static void save_ppm(const char* path, int w, int h) {
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--) std::fwrite(pixels.data() + static_cast<size_t>(y) * w * 3, 1, static_cast<size_t>(w) * 3, f);
        std::fclose(f);
    }
}

int main(int argc, char** argv) {
    int opt_sample = 0, opt_steps = 0, opt_hazard = -1, opt_penalty = -1;
    int opt_inspect = -1, opt_lesson = -1, opt_next = 0, opt_challenge = 0, opt_answer = -1, opt_theme = -1;
    float opt_scale = 0.0f;
    const char* opt_shot = nullptr;
    const char* opt_window = "";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--sample")) opt_sample = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--steps")) opt_steps = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--hazard")) opt_hazard = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--penalty")) opt_penalty = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--screenshot")) opt_shot = argv[i + 1];
        else if (!std::strcmp(argv[i], "--inspect")) opt_inspect = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--lesson")) opt_lesson = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--next")) opt_next = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--challenge")) opt_challenge = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--answer")) opt_answer = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--theme")) opt_theme = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--scale")) opt_scale = static_cast<float>(std::atof(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--window")) opt_window = argv[i + 1];
    }

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    if (!opt_shot) glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "MIPS Pipeline Simulator", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    float xs = 1.0f, ys = 1.0f;
    glfwGetWindowContentScale(window, &xs, &ys);
    if (opt_shot || xs < 1.0f) xs = 1.0f;

    App app;
    app.dpi_scale = xs;
    app.persist = opt_shot == nullptr;
    app.load_settings();
    if (opt_theme >= 0 && opt_theme <= 2) app.settings.theme = opt_theme;
    if (opt_scale >= 0.7f && opt_scale <= 2.0f) app.settings.ui_scale = opt_scale;
    app.scale_edit = app.settings.ui_scale * 100.0f;
    int gmaj, gmin, grev;
    glfwGetVersion(&gmaj, &gmin, &grev);
    app.backend_info = "GLFW " + std::to_string(gmaj) + "." + std::to_string(gmin) + "." + std::to_string(grev) + ", OpenGL";

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");
    restyle(app, true);

    if (opt_sample > 0 && opt_sample < SAMPLE_COUNT) app.load_sample(opt_sample);
    if (opt_hazard >= 0 && opt_hazard <= 2) app.sim.cfg.hazard = static_cast<HazardMode>(opt_hazard);
    if (opt_penalty >= 0) app.sim.cfg.miss_penalty = opt_penalty;
    app.show_settings = !std::strcmp(opt_window, "settings");
    app.show_about = !std::strcmp(opt_window, "about");

    double last = glfwGetTime();
    int frame = 0;
    double shot_at = -1.0;
    while (!glfwWindowShouldClose(window) && !app.want_quit) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            glfwWaitEventsTimeout(0.1);
            continue;
        }
        if (app.restyle) restyle(app, false);  // must happen between frames

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const double now = glfwGetTime();
        if (frame == 3) {
            if (opt_lesson >= 0 && opt_lesson < static_cast<int>(lessons().size())) {
                app.start_lesson(opt_lesson);
                for (int i = 0; i < opt_next; i++) {
                    app.lesson_next();
                    while (app.lesson_running) app.lesson_tick(now);
                }
            }
            for (int i = 0; i < opt_steps; i++) app.step(now);
            if (opt_inspect >= 0 && opt_inspect < 5) app.select(Selection::Stage, opt_inspect);
            if (opt_challenge) {
                app.challenge = true;
                for (int i = 0; i < 200 && !app.quiz.active && !app.sim.halted(); i++) app.advance(now);
                if (opt_answer >= 0) app.answer(opt_answer);
            }
            shot_at = now + 0.45;
        }
        app.frame(now, static_cast<float>(now - last));
        last = now;

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(app.clear_color[0], app.clear_color[1], app.clear_color[2], 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (opt_shot && frame > 5 && now >= shot_at) {
            save_ppm(opt_shot, w, h);
            break;
        }
        glfwSwapBuffers(window);
        frame++;
    }
    app.save_settings();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
