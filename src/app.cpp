#include "midori/app.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include <format>
#include <glm/gtc/type_ptr.hpp>
#include <numbers>
#include <random>

#if defined(NDEBUG) && defined(TRACY_ENABLE)
#undef TRACY_ENABLE
#endif
#include <tracy/Tracy.hpp>

#include "SDL3/SDL_log.h"

namespace Midori {

App::App(int argc, char *argv[]) : renderer(this), canvas(this) {
    ZoneScoped;
    args.resize(argc);
    for (size_t i = 0; i < args.size(); i++) {
        args[i] = std::string(argv[i]);
    }
}

bool App::Init() {
    ZoneScoped;

    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (main_scale == 0.0F) {
        main_scale = 1.0F;
    }

    window = SDL_CreateWindow("Midori", window_size.x, window_size.y,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to create window: %s", SDL_GetError());
        return false;
    }
    SDL_SetWindowPosition(window, window_pos.x, window_pos.y);
    SDL_ShowWindow(window);

    SDL_GetMouseState(&cursor_current_pos.x, &cursor_current_pos.y);
    cursor_last_pos = cursor_current_pos;

    if (!renderer.Init()) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize renderer");
        return false;
    }

    if (!canvas.Open()) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to create new canvas");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/Segoeui.ttf", 16.0f);

    ImGui::StyleColorsLight();

    ImGuiStyle &style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;
    // TODO: load cascadia font

    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo init_info = {
        .Device = renderer.device,
        .ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(renderer.device, window),
        .MSAASamples = SDL_GPU_SAMPLECOUNT_1,
        .SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
        .PresentMode = SDL_GPU_PRESENTMODE_VSYNC,
    };
    ImGui_ImplSDLGPU3_Init(&init_info);

    return true;
}

void DebugDrawTile(ImDrawList *draw_list, glm::ivec2 pos) {}

static const SDL_DialogFileFilter filters[] = {{"Midori files (.mido)", "mido"}, {"All files", "*"}};

bool App::Update() {
    FrameMark;
    // std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (window_size.x > 0 && window_size.y > 0 && !hidden) {
        {  // ImGui Stuff
            ZoneScopedN("UI");
            ImGui_ImplSDLGPU3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            static bool ui_debug_culling = false;
            auto layer = canvas.selected_layer;
            if (canvas.stroke_layer > 0) {
                layer = canvas.stroke_layer;
            }
            ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
            if (ui_debug_culling && layer > 0) {
                draw_list->AddRect(
                    ImVec2{
                        static_cast<float>(window_size.x - window_size.x / 2) / 2.0f,
                        static_cast<float>(window_size.y - window_size.y / 2) / 2.0f,
                    },
                    ImVec2{
                        static_cast<float>(window_size.x + window_size.x / 2) / 2.0f,
                        static_cast<float>(window_size.y + window_size.y / 2) / 2.0f,
                    },
                    IM_COL32(0, 255, 0, 64));
                for (const auto pos : canvas.viewport.VisibleTiles(window_size / 2)) {
                    ImVec2 points[5]{};
                    ImVec2 center;
                    glm::vec2 offsets[5] = {
                        {1.0f, 1.0f},
                        {static_cast<float>(TILE_SIZE), 0.0f},
                        {static_cast<float>(TILE_SIZE), static_cast<float>(TILE_SIZE)},
                        {1.0f, static_cast<float>(TILE_SIZE)},
                        {1.0f, 1.0f},
                    };
                    for (size_t i = 0; i < 5; i++) {
                        glm::vec4 p =
                            canvas.viewport.ViewMatrix() *
                            glm::vec4(static_cast<glm::vec2>(pos) * static_cast<float>(TILE_SIZE) + offsets[i], 0.0f,
                                      1.0f);

                        points[i] = ImVec2{
                            static_cast<float>(window_size.x) / 2.0f + p.x,
                            static_cast<float>(window_size.y) / 2.0f + p.y,
                        };
                        center.x += points[i].x / 5.0f;
                        center.y += points[i].y / 5.0f;
                    }

                    draw_list->AddPolyline(points, 5, IM_COL32(0, 0, 0, 64), 0, 1.0f);
                    std::string text = std::format("[{}, {}]", pos.x, pos.y);
                    draw_list->AddText(center, IM_COL32(0, 0, 0, 64), text.c_str());
                }
            }

            canvas.viewport.UI();

            if (!ui_focus) {
                ImVec2 pos = {};
                float radius = 0.0f;
                // if (!canvas.stroke_points.empty()) {
                //     Canvas::StrokePoint p = canvas.previous_point;
                //     if (canvas.brush_mode) {
                //         p = canvas.ApplyBrushPressure(p, 0.5f);
                //     } else {
                //         p = canvas.ApplyEraserPressure(p, pen_pressure);
                //     }

                //     pos.x = ((float)window_size.x / 2.0f) + p.position.x + ((float)p.position.x * (float)TILE_SIZE);
                //     pos.y = ((float)window_size.y / 2.0f) + p.position.y + ((float)p.position.y * (float)TILE_SIZE);
                //     radius = p.radius;
                // }
                if (!canvas.stroke_started) {
                    pos.x = cursor_current_pos.x;
                    pos.y = cursor_current_pos.y;
                    if (canvas.brush_mode) {
                        radius = canvas.brush_options.radius;
                    } else {
                        radius = canvas.eraser_options.radius;
                    }
                    draw_list->AddCircle(pos, radius, ImColor(0, 0, 0, 255));
                }
            }

            if (ImGui::Begin("Layers")) {
                // Temporary layer stuff
                std::vector<LayerInfo> layer_info;
                layer_info.reserve(canvas.layer_infos.size());
                for (auto &[layer, info] : canvas.layer_infos) {
                    layer_info.push_back(info);
                }

                std::ranges::sort(layer_info, [](LayerInfo &a, LayerInfo &b) { return a.depth > b.depth; });

                const ImVec2 image_size = {
                    (float)window_size.x / (float)window_size.y * 50.0f,
                    50.0f,
                };
                for (int i = layer_info.size() - 1; i >= 0; i--) {
                    auto &real_data = canvas.layer_infos[layer_info[i].layer];
                    const std::string title = std::format("-{}: {}", real_data.depth, real_data.name);
                    ImGui::SeparatorText(title.c_str());
                    ImGui::PushID(real_data.layer);

                    bool layer_selected = real_data.layer == canvas.selected_layer;
                    if (ImGui::Checkbox("Selected", &layer_selected)) {
                        canvas.selected_layer = real_data.layer;
                    }
                    ImGui::SliderFloat("", &real_data.opacity, 0.0f, 1.0f);

                    // ImGui::Image(
                    //     (ImTextureID)(intptr_t)renderer.layer_textures.at(real_data.layer),
                    //     image_size);

                    if (real_data.depth < (canvas.current_max_layer_height - 1)) {
                        ImGui::SameLine();
                        if (ImGui::Button("-")) {
                            canvas.SetLayerDepth(real_data.layer, real_data.depth + 1);
                        }
                    }
                    if (real_data.depth > 0) {
                        ImGui::SameLine();
                        if (ImGui::Button("+")) {
                            canvas.SetLayerDepth(real_data.layer, real_data.depth - 1);
                        }
                    }
                    if (i != 0) {
                        ImGui::SameLine();
                        if (ImGui::Button("=")) {
                            // canvas.MergeLayers(real_data.layer, layer_info[i - 1].layer);
                            // canvas.DeleteLayer(real_data.layer);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("x")) {
                        canvas.DeleteLayer(real_data.layer);
                    }

                    ImGui::PopID();
                }
                if (ImGui::Button("+")) {
                    const auto layer = canvas.CreateLayer("New Layer", 0);
                    canvas.SaveLayer(layer);
                }
                ImGui::ColorEdit4("Background Color", glm::value_ptr(bg_color));
            }
            ImGui::End();

            // TODO: Debug tile multithreaded download process (GPU->CPU->Disk)
            // TODO: Debug tile multithreaded upload process (Disk->CPU->GPU)

            if (ImGui::Begin("Stroke Options")) {
                if (ImGui::Checkbox("Eraser", &canvas.eraser_mode)) {
                    canvas.brush_mode = !canvas.eraser_mode;
                }

                if (canvas.eraser_mode) {
                    {
                        ImGui::PushID("Opacity");
                        if (ImGui::SliderFloat("Opacity", &canvas.eraser_options.opacity, 0.0f, 1.0f)) {
                            canvas.eraser_options.opacity = std::ceil(canvas.eraser_options.opacity * 100.0f) / 100.0f;
                            canvas.eraser_options_modified = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Checkbox("Pen", &canvas.eraser_options.opacity_pressure)) {
                            canvas.eraser_options_modified = true;
                        }
                        if (canvas.eraser_options.opacity_pressure) {
                            if (ImGui::SliderFloat2("range",
                                                    glm::value_ptr(canvas.eraser_options.opacity_pressure_range), 0.0f,
                                                    1.0f)) {
                                canvas.eraser_options.opacity_pressure_range =
                                    glm::ceil(canvas.eraser_options.opacity_pressure_range * 100.0f) / 100.0f;
                                canvas.eraser_options.opacity_pressure_range.x =
                                    std::min(canvas.eraser_options.opacity_pressure_range.x,
                                             canvas.eraser_options.opacity_pressure_range.y);
                                canvas.eraser_options.opacity_pressure_range.y =
                                    std::max(canvas.eraser_options.opacity_pressure_range.x,
                                             canvas.eraser_options.opacity_pressure_range.y);
                                canvas.eraser_options_modified = true;
                            }
                        }
                        ImGui::PopID();
                    }
                    {
                        ImGui::PushID("Radius");
                        if (ImGui::SliderFloat("Radius", &canvas.eraser_options.radius, 0.0f, 100.0f)) {
                            canvas.eraser_options.radius = std::ceil(canvas.eraser_options.radius * 100.0f) / 100.0f;
                            canvas.eraser_options_modified = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Checkbox("Pen", &canvas.eraser_options.radius_pressure)) {
                            canvas.eraser_options_modified = true;
                        }
                        if (canvas.eraser_options.radius_pressure) {
                            if (ImGui::SliderFloat2("Radius Range",
                                                    glm::value_ptr(canvas.eraser_options.radius_pressure_range), 0.0f,
                                                    1.0f)) {
                                canvas.eraser_options.radius_pressure_range =
                                    glm::ceil(canvas.eraser_options.radius_pressure_range * 100.0f) / 100.0f;
                                canvas.eraser_options.radius_pressure_range.x =
                                    std::min(canvas.eraser_options.radius_pressure_range.x,
                                             canvas.eraser_options.radius_pressure_range.y);
                                canvas.eraser_options.radius_pressure_range.y =
                                    std::max(canvas.eraser_options.radius_pressure_range.x,
                                             canvas.eraser_options.radius_pressure_range.y);
                                canvas.eraser_options_modified = true;
                            }
                        }
                        ImGui::PopID();
                    }
                    {
                        ImGui::PushID("Flow");
                        if (ImGui::SliderFloat("Flow", &canvas.eraser_options.flow, 0.01f, 1.0f)) {
                            canvas.eraser_options.flow = std::ceil(canvas.eraser_options.flow * 100.0f) / 100.0f;
                            canvas.eraser_options_modified = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Checkbox("Pen", &canvas.eraser_options.flow_pressure)) {
                            canvas.eraser_options_modified = true;
                        }
                        if (canvas.eraser_options.flow_pressure) {
                            if (ImGui::SliderFloat2("range", glm::value_ptr(canvas.eraser_options.flow_pressure_range),
                                                    0.0f, 1.0f)) {
                                canvas.eraser_options.flow_pressure_range =
                                    glm::ceil(canvas.eraser_options.flow_pressure_range * 100.0f) / 100.0f;
                                canvas.eraser_options.flow_pressure_range.x =
                                    std::min(canvas.eraser_options.flow_pressure_range.x,
                                             canvas.eraser_options.flow_pressure_range.y);
                                canvas.eraser_options.flow_pressure_range.y =
                                    std::max(canvas.eraser_options.flow_pressure_range.x,
                                             canvas.eraser_options.flow_pressure_range.y);
                                canvas.eraser_options_modified = true;
                            }
                        }
                        ImGui::PopID();
                    }
                    {
                        ImGui::PushID("Hardness");
                        if (ImGui::SliderFloat("Hardness", &canvas.eraser_options.hardness, 0.0f, 1.0f)) {
                            canvas.eraser_options.hardness =
                                std::ceil(canvas.eraser_options.hardness * 100.0f) / 100.0f;
                            canvas.eraser_options_modified = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Checkbox("Pen", &canvas.eraser_options.hardness_pressure)) {
                            canvas.eraser_options_modified = true;
                        }
                        if (canvas.eraser_options.hardness_pressure) {
                            if (ImGui::SliderFloat2("range",
                                                    glm::value_ptr(canvas.eraser_options.hardness_pressure_range), 0.0f,
                                                    1.0f)) {
                                canvas.eraser_options.hardness_pressure_range =
                                    glm::ceil(canvas.eraser_options.hardness_pressure_range * 100.0f) / 100.0f;
                                canvas.eraser_options.hardness_pressure_range.x =
                                    std::min(canvas.eraser_options.hardness_pressure_range.x,
                                             canvas.eraser_options.hardness_pressure_range.y);
                                canvas.eraser_options.hardness_pressure_range.y =
                                    std::max(canvas.eraser_options.hardness_pressure_range.x,
                                             canvas.eraser_options.hardness_pressure_range.y);
                                canvas.eraser_options_modified = true;
                            }
                        }
                        ImGui::PopID();
                    }
                    if (ImGui::SliderFloat("Spacing", &canvas.eraser_options.spacing, 0.1f, 50.0f)) {
                        canvas.eraser_options.spacing = std::ceil(canvas.eraser_options.spacing * 100.0f) / 100.0f;
                        canvas.eraser_options_modified = true;
                    }
                    ImGui::Separator();
                    ImGui::LabelText("Stroke num", "%llu", canvas.stroke_points.size());
                    ImGui::Checkbox("Using pen", &pen_in_range);
                    ImGui::LabelText("Cursor", "x: %.2f, y: %.2f", cursor_current_pos.x, cursor_current_pos.y);
                    ImGui::LabelText("Pressure", "%.2f", pen_pressure);
                } else if (canvas.brush_mode) {
                    {
                        ImGui::PushID("Opacity");
                        if (ImGui::ColorEdit4("Color", glm::value_ptr(canvas.brush_options.color))) {
                            canvas.brush_options_modified = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Checkbox("Pen", &canvas.brush_options.opacity_pressure)) {
                            canvas.brush_options_modified = true;
                        }
                        if (canvas.brush_options.opacity_pressure) {
                            if (ImGui::SliderFloat2(
                                    "range", glm::value_ptr(canvas.brush_options.opacity_pressure_range), 0.0f, 1.0f)) {
                                canvas.brush_options.opacity_pressure_range =
                                    glm::ceil(canvas.brush_options.opacity_pressure_range * 100.0f) / 100.0f;
                                canvas.brush_options.opacity_pressure_range.x =
                                    std::min(canvas.brush_options.opacity_pressure_range.x,
                                             canvas.brush_options.opacity_pressure_range.y);
                                canvas.brush_options.opacity_pressure_range.y =
                                    std::max(canvas.brush_options.opacity_pressure_range.x,
                                             canvas.brush_options.opacity_pressure_range.y);
                                canvas.brush_options_modified = true;
                            }
                        }
                        ImGui::PopID();
                    }
                    {
                        ImGui::PushID("Radius");
                        if (ImGui::SliderFloat("Radius", &canvas.brush_options.radius, 0.0f, 100.0f)) {
                            canvas.brush_options.radius = std::ceil(canvas.brush_options.radius * 100.0f) / 100.0f;
                            canvas.brush_options_modified = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Checkbox("Pen", &canvas.brush_options.radius_pressure)) {
                            canvas.brush_options_modified = true;
                        }
                        if (canvas.brush_options.radius_pressure) {
                            if (ImGui::SliderFloat2("range", glm::value_ptr(canvas.brush_options.radius_pressure_range),
                                                    0.0f, 1.0f)) {
                                canvas.brush_options.radius_pressure_range =
                                    glm::ceil(canvas.brush_options.radius_pressure_range * 100.0f) / 100.0f;
                                canvas.brush_options.radius_pressure_range.x =
                                    std::min(canvas.brush_options.radius_pressure_range.x,
                                             canvas.brush_options.radius_pressure_range.y);
                                canvas.brush_options.radius_pressure_range.y =
                                    std::max(canvas.brush_options.radius_pressure_range.x,
                                             canvas.brush_options.radius_pressure_range.y);
                                canvas.brush_options_modified = true;
                            }
                        }
                        ImGui::PopID();
                    }
                    {
                        ImGui::PushID("Flow");
                        if (ImGui::SliderFloat("Flow", &canvas.brush_options.flow, 0.01f, 1.0f)) {
                            canvas.brush_options.flow = std::ceil(canvas.brush_options.flow * 100.0f) / 100.0f;
                            canvas.brush_options_modified = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Checkbox("Pen", &canvas.brush_options.flow_pressure)) {
                            canvas.brush_options_modified = true;
                        }
                        if (canvas.brush_options.flow_pressure) {
                            if (ImGui::SliderFloat2("range", glm::value_ptr(canvas.brush_options.flow_pressure_range),
                                                    0.0f, 1.0f)) {
                                canvas.brush_options.flow_pressure_range =
                                    glm::ceil(canvas.brush_options.flow_pressure_range * 100.0f) / 100.0f;
                                canvas.brush_options.flow_pressure_range.x =
                                    std::min(canvas.brush_options.flow_pressure_range.x,
                                             canvas.brush_options.flow_pressure_range.y);
                                canvas.brush_options.flow_pressure_range.y =
                                    std::max(canvas.brush_options.flow_pressure_range.x,
                                             canvas.brush_options.flow_pressure_range.y);
                                canvas.brush_options_modified = true;
                            }
                        }
                        ImGui::PopID();
                    }
                    {
                        ImGui::PushID("Hardness");
                        if (ImGui::SliderFloat("Hardness", &canvas.brush_options.hardness, 0.0f, 1.0f)) {
                            canvas.brush_options.hardness = std::ceil(canvas.brush_options.hardness * 100.0f) / 100.0f;
                            canvas.brush_options_modified = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Checkbox("Pen", &canvas.brush_options.hardness_pressure)) {
                            canvas.brush_options_modified = true;
                        }
                        if (canvas.brush_options.hardness_pressure) {
                            if (ImGui::SliderFloat2("range",
                                                    glm::value_ptr(canvas.brush_options.hardness_pressure_range), 0.0f,
                                                    1.0f)) {
                                canvas.brush_options.hardness_pressure_range =
                                    glm::ceil(canvas.brush_options.hardness_pressure_range * 100.0f) / 100.0f;
                                canvas.brush_options.hardness_pressure_range.x =
                                    std::min(canvas.brush_options.hardness_pressure_range.x,
                                             canvas.brush_options.hardness_pressure_range.y);
                                canvas.brush_options.hardness_pressure_range.y =
                                    std::max(canvas.brush_options.hardness_pressure_range.x,
                                             canvas.brush_options.hardness_pressure_range.y);
                                canvas.brush_options_modified = true;
                            }
                        }
                        ImGui::PopID();
                    }
                    if (ImGui::SliderFloat("Spacing", &canvas.brush_options.spacing, 0.1f, 50.0f)) {
                        canvas.brush_options.spacing = std::ceil(canvas.brush_options.spacing * 100.0f) / 100.0f;
                        canvas.brush_options_modified = true;
                    }
                    ImGui::Separator();
                    ImGui::LabelText("Stroke num", "%llu", canvas.stroke_points.size());
                    ImGui::Checkbox("Using pen", &pen_in_range);
                    ImGui::LabelText("Cursor", "x: %.2f, y: %.2f", cursor_current_pos.x, cursor_current_pos.y);
                    ImGui::LabelText("Pressure", "%.2f", pen_pressure);
                }
            }
            ImGui::End();

            if (ImGui::Begin("Canvas History")) {
                if (canvas.canvasHistory.size() > 0) {
                    for (size_t pos = 0; pos < canvas.canvasHistory.size(); pos++) {
                        const Command *command = canvas.canvasHistory.get(pos);
                        const auto name = command->name();
                        if (pos < canvas.canvasHistory.position()) {
                            ImGui::TextColored(ImColor(0, 0, 0), "[%llu]: %s", pos, name.c_str());
                        } else if (pos == canvas.canvasHistory.position()) {
                            ImGui::TextColored(ImColor(0, 128, 0), "[%llu]: %s", pos, name.c_str());
                        } else if (pos > canvas.canvasHistory.position()) {
                            ImGui::TextColored(ImColor(0, 128, 128), "[%llu]: %s", pos, name.c_str());
                        }
                    }
                } else {
                    ImGui::TextColored(ImColor(0, 0, 128), "Nothing");
                }
            }
            ImGui::End();
            if (ImGui::Begin("View History")) {
                if (canvas.viewHistory.size() > 0) {
                    for (size_t pos = 0; pos < canvas.viewHistory.size(); pos++) {
                        const Command *command = canvas.viewHistory.get(pos);
                        const auto name = command->name();
                        if (pos < canvas.viewHistory.position()) {
                            ImGui::TextColored(ImColor(0, 0, 0), "[%llu]: %s", pos, name.c_str());
                        } else if (pos == canvas.viewHistory.position()) {
                            ImGui::TextColored(ImColor(0, 128, 0), "[%llu]: %s", pos, name.c_str());
                        } else if (pos > canvas.viewHistory.position()) {
                            ImGui::TextColored(ImColor(0, 128, 128), "[%llu]: %s", pos, name.c_str());
                        }
                    }
                } else {
                    ImGui::TextColored(ImColor(0, 0, 128), "Nothing");
                }
            }
            ImGui::End();
        }

        ImGui::Render();
    }

    canvas.Update();

    renderer.Render();

    canvas.DeleteUpdate();

    return true;
}

bool App::Resize(const int width, const int height) {
    ZoneScoped;
    window_size.x = width;
    window_size.y = height;

    if (window_size.x > 0 || window_size.y > 0) {
        if (!renderer.Resize()) {
            return false;
        }
    }

    return true;
}

void App::ShouldQuit() {
    should_quit = true;

    canvas.canvasHistory.clear();

    const std::vector<Layer> layersToSave(canvas.layersModified.begin(), canvas.layersModified.end());
    if (canvas.brush_options_modified) {
        canvas.SaveBrush();
    }
    if (canvas.eraser_options_modified) {
        canvas.SaveEraser();
    }

    for (const auto &layer : layersToSave) {
        if (canvas.layer_infos[layer].temporary) {
            continue;
        }

        canvas.SaveLayer(layer);
    }

    for (const auto &layer : canvas.Layers()) {
        if (canvas.layer_infos.at(layer).temporary) {
            canvas.DeleteLayer(layer);
        }

        for (const auto &tile : canvas.LayerTiles(layer)) {
            canvas.UnloadTile(layer, tile);
        }
    }
}

bool App::CanQuit() { return canvas.CanQuit() && renderer.CanQuit(); }

void App::Quit() {
    ZoneScoped;

    SDL_WaitForGPUIdle(renderer.device);

    ImGui_ImplSDL3_Shutdown();
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui::DestroyContext();
    renderer.Quit();

    SDL_DestroyWindow(window);
    window = nullptr;
}

void App::CursorMove(glm::vec2 new_pos) {
    // new_pos = canvas.viewport.ScreenToCanvas(new_pos, window_size);

    static glm::vec2 last_pos = cursor_last_pos;

    if (changeCursorSize) {
        glm::vec2 delta_pos = new_pos - last_pos;
        last_pos = new_pos;

        canvas.ChangeRadiusSize(delta_pos, shift_pressed);
        return;
    }

    cursor_last_pos = cursor_current_pos;
    cursor_current_pos = new_pos;
    cursor_delta_pos = cursor_current_pos - cursor_last_pos;
    last_pos = cursor_last_pos;

    if (!cursor_left_pressed) {
        canvas.viewCursorStart = new_pos;
    }

    if (cursor_left_pressed) {
        if ((canvas.viewPanning || canvas.viewRotating || canvas.viewZooming) && (glm::length(cursor_delta_pos) > 0) &&
            canvas.currentViewportChangeCommand == nullptr) {
            canvas.currentViewportChangeCommand = std::make_unique<ViewportChangeCommand>(*this);
            canvas.currentViewportChangeCommand->SetPreviousViewport(canvas.viewport);
            SDL_Log("dqsbjdbjqsbdjqbsjlmd");
        }

        canvas.ViewUpdateCursor(cursor_current_pos);

        if (canvas.stroke_started) {
            if (canvas.brush_mode) {
                canvas.UpdateBrushStroke(Canvas::StrokePoint{
                    .color = canvas.brush_options.color,
                    .position = canvas.viewport.ScreenToCanvas(cursor_current_pos, window_size),
                    .radius = canvas.brush_options.radius,
                    .flow = canvas.brush_options.flow,
                    .hardness = canvas.brush_options.hardness,
                });
            } else if (canvas.eraser_mode) {
                canvas.UpdateEraserStroke(Canvas::StrokePoint{
                    .color = {0.0f, 0.0f, 0.0f, canvas.eraser_options.opacity},
                    .position = canvas.viewport.ScreenToCanvas(cursor_current_pos, window_size),
                    .radius = canvas.eraser_options.radius,
                    .flow = canvas.eraser_options.flow,
                    .hardness = canvas.eraser_options.hardness,
                });
            }
        }
    }

    canvas.viewCursorPrevious = new_pos;

    cursor_delta_pos = glm::vec2(0.0f);
}
void App::CursorPress(Uint8 button) {
    if (button == SDL_BUTTON_LEFT) {
        cursor_left_pressed = true;

        if (!canvas.viewPanning && !canvas.viewZooming && !canvas.viewRotating && (canvas.selected_layer != 0) &&
            !canvas.stroke_started) {
            if (canvas.brush_mode) {
                canvas.StartBrushStroke(Canvas::StrokePoint{
                    .color = canvas.brush_options.color,
                    .position = canvas.viewport.ScreenToCanvas(cursor_current_pos, window_size),
                    .radius = canvas.brush_options.radius,
                    .flow = canvas.brush_options.flow,
                    .hardness = canvas.brush_options.hardness,
                });
            } else if (canvas.eraser_mode) {
                canvas.StartEraserStroke(Canvas::StrokePoint{
                    .color = {0.0f, 0.0f, 0.0f, canvas.eraser_options.opacity},
                    .position = canvas.viewport.ScreenToCanvas(cursor_current_pos, window_size),
                    .radius = canvas.eraser_options.radius,
                    .flow = canvas.eraser_options.flow,
                    .hardness = canvas.eraser_options.hardness,
                });
            }
        }
    }
}
void App::CursorRelease(Uint8 button) {
    if (button == SDL_BUTTON_LEFT) {
        cursor_left_pressed = false;
        if (!canvas.viewPanning && !canvas.viewZooming && !canvas.viewRotating && (canvas.selected_layer != 0) &&
            canvas.stroke_started) {
            if (canvas.brush_mode) {
                canvas.EndBrushStroke(Canvas::StrokePoint{
                    .color = canvas.brush_options.color,
                    .position = canvas.viewport.ScreenToCanvas(cursor_current_pos, window_size),
                    .radius = canvas.brush_options.radius,
                    .flow = canvas.brush_options.flow,
                    .hardness = canvas.brush_options.hardness,
                });
            } else if (canvas.eraser_mode) {
                canvas.EndEraserStroke(Canvas::StrokePoint{
                    .color = {0.0f, 0.0f, 0.0f, canvas.eraser_options.opacity},
                    .position = canvas.viewport.ScreenToCanvas(cursor_current_pos, window_size),
                    .radius = canvas.eraser_options.radius,
                    .flow = canvas.eraser_options.flow,
                    .hardness = canvas.eraser_options.hardness,
                });
            }
        }
    }
    if (button == SDL_BUTTON_RIGHT) {
        cursor_right_pressed = false;
    }
}

// Need to detect when a mods is disengaged
void App::KeyPress(SDL_Keycode key, SDL_Keymod mods) {
    if (key == SDLK_SPACE) {
        space_pressed = true;
    }
    if (key == SDLK_LCTRL || key == SDLK_RCTRL) {
        ctrl_pressed = true;
    }
    if (key == SDLK_LSHIFT || key == SDLK_RSHIFT) {
        shift_pressed = true;
    }
    if (key == SDLK_LALT || key == SDLK_RALT) {
        alt_pressed = true;
    }

    if (changeCursorSize) {
        return;
    }

    if (key == SDLK_F11) {
        Fullscreen(!fullscreen);
    }
    if (key == SDLK_F && ctrl_pressed) {
        canvas.viewport.FlipHorizontal();
    }
    if (key == SDLK_G && alt_pressed) {
        canvas.viewport.SetRotation(0.0f);
        canvas.viewport.SetZoom(glm::vec2(0.0f), glm::vec2(1.0f));
    }

    if (!canvas.stroke_started) {
        if (key == SDLK_F && !ctrl_pressed) {
            changeCursorSize = true;
        }

        if (key == SDLK_B && canvas.eraser_mode) {
            canvas.eraser_mode = false;
            canvas.brush_mode = true;
        }
        if (key == SDLK_E && canvas.brush_mode) {
            canvas.brush_mode = false;
            canvas.eraser_mode = true;
        }

        if (key == SDLK_Z && ctrl_pressed) {
            if (shift_pressed) {
                canvas.canvasHistory.redo();
            } else {
                canvas.canvasHistory.undo();
            }
        }

        if (key == SDLK_LEFT && alt_pressed) {
            canvas.viewHistory.undo();
        }
        if (key == SDLK_RIGHT && alt_pressed) {
            canvas.viewHistory.redo();
        }
    }

    canvas.ViewUpdateState(cursor_current_pos);
}

void App::KeyRelease(SDL_Keycode key, SDL_Keymod mods) {
    if (key == SDLK_SPACE) {
        space_pressed = false;
        if (canvas.currentViewportChangeCommand != nullptr) {
            canvas.currentViewportChangeCommand->SetNewViewport(canvas.viewport);
            canvas.viewHistory.store(std::move(canvas.currentViewportChangeCommand));
            canvas.currentViewportChangeCommand = nullptr;
        }
    }
    if (key == SDLK_LCTRL || key == SDLK_RCTRL) {
        ctrl_pressed = false;
    }
    if (key == SDLK_LSHIFT || key == SDLK_RSHIFT) {
        shift_pressed = false;
    }
    if (key == SDLK_LALT || key == SDLK_RALT) {
        alt_pressed = false;
    }

    if (changeCursorSize && key == SDLK_F) {
        changeCursorSize = false;
        return;
    }

    canvas.ViewUpdateState(cursor_current_pos);
}

void App::Fullscreen(bool enable) {
    SDL_SetWindowFullscreen(window, enable);
    fullscreen = enable;
}

}  // namespace Midori