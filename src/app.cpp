#include "app.h"

#include <SDL3/SDL_log.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlgpu3.h>
#include <format>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <numbers>
#include <random>
#include <tracy/Tracy.hpp>

namespace Midori {

App::App(int argc, char* argv[]) : renderer(this), canvas(this), ui(*this) {
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

    ui.Init();

    canvas.viewport.Resize(glm::vec2(window_size));
    if (!canvas.Open()) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to create new canvas");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/Segoeui.ttf", 16.0f);

    ImGui::StyleColorsLight();

    ImGuiStyle& style = ImGui::GetStyle();
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

static const SDL_DialogFileFilter filters[] = {{"Midori files (.mido)", "mido"}, {"All files", "*"}};

bool App::Update() {
    // std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (window_size.x > 0 && window_size.y > 0 && !hidden) {
        { // ImGui Stuff
            ZoneScopedN("UI");
            ImGui_ImplSDLGPU3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            static bool ui_debug_culling = false;
            auto layer = canvas.selectedLayer;
            if (canvas.strokeLayer > 0) {
                layer = canvas.strokeLayer;
            }
            ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

            // DebugTileCulling(glm::vec2(window_size) / 2.0f, draw_list);

            if (ui_debug_culling && layer > 0) {
                for (const auto& [pos, tile] : canvas.layerTilePos[layer]) {
                    if (canvas.tile_read_queue.contains(tile)) {
                        ImU32 col = IM_COL32(255, 0, 0, 64);
                        const char* str{};
                        switch (canvas.tile_read_queue.at(tile).state) {
                        case Canvas::TileReadState::Queued:
                            DrawTileDebug(pos, draw_list, col, "TileReadState::Queued");
                            break;
                        case Canvas::TileReadState::Read:
                            DrawTileDebug(pos, draw_list, col, "TileReadState::Read");
                            break;
                        case Canvas::TileReadState::Decompressed:
                            DrawTileDebug(pos, draw_list, col, "TileReadState::Decompressed");
                            break;
                        case Canvas::TileReadState::Uploaded:
                            DrawTileDebug(pos, draw_list, col, "TileReadState::Uploaded");
                            break;
                        default:
                            DrawTileDebug(pos, draw_list, col, "TileReadState::N/A");
                            break;
                        }
                    } else if (canvas.tile_write_queue.contains(tile)) {
                        ImU32 col = IM_COL32(0, 255, 0, 64);
                        const char* str{};
                        switch (canvas.tile_write_queue.at(tile).state) {
                        case Canvas::TileWriteState::Queued:
                            DrawTileDebug(pos, draw_list, col, "TileReadState::Queued");
                            break;
                        case Canvas::TileWriteState::Downloading:
                            DrawTileDebug(pos, draw_list, col, "TileReadState::Downloading");
                            break;
                        case Canvas::TileWriteState::Downloaded:
                            DrawTileDebug(pos, draw_list, col, "TileReadState::Downloaded");
                            break;
                        case Canvas::TileWriteState::Encoded:
                            DrawTileDebug(pos, draw_list, col, "TileReadState::Encoded");
                            break;
                        case Canvas::TileWriteState::Written:
                            DrawTileDebug(pos, draw_list, col, "TileReadState::Written");
                            break;
                        default:
                            DrawTileDebug(pos, draw_list, col, "TileReadState::N/A");
                            break;
                        }
                    } else {
                        ImU32 col = IM_COL32(0, 0, 0, 64);
                        DrawTileDebug(pos, draw_list, col);
                    }
                }
            }

            if (!ui_focus) {
                ImVec2 pos = {};
                ImVec2 radius = {};

                if (!canvas.stroke_started) {
                    pos.x = cursor_current_pos.x;
                    pos.y = cursor_current_pos.y;
                    if (canvas.brushMode) {
                        radius.x = canvas.brushOptions.radius * canvas.viewport.Zoom().x;
                        radius.y = canvas.brushOptions.radius * canvas.viewport.Zoom().y;
                    } else {
                        radius.x = canvas.eraserOptions.radius * canvas.viewport.Zoom().x;
                        radius.y = canvas.eraserOptions.radius * canvas.viewport.Zoom().y;
                    }
                    draw_list->AddEllipse(pos, radius, ImColor(0, 0, 0, 255));
                }
            }

            if (!hide_ui) {
                canvas.viewport.UI();

                if (ImGui::Begin("Debug")) {
                    ImGui::Checkbox("view loaded tiles", &ui_debug_culling);

                    size_t tileModified{};
                    size_t tileSaved{};
                    for (const auto& [la, infos] : canvas.layerInfos) {
                        tileModified += canvas.layerTilesModified.at(la).size();
                        // tileSaved += canvas.layerTilesSaved.at(layer).size();
                    }

                    ImGui::LabelText("tile loaded", "%zu", canvas.tileInfos.size());
                    ImGui::LabelText("tile textures", "%zu", renderer.tile_textures.size());
                    ImGui::LabelText("tile modified", "%zu", tileModified);
                    ImGui::LabelText("tile saved", "%zu", 0);
                }
                ImGui::End();

                if (ImGui::Begin("Layers")) {
                    // Temporary layer stuff
                    eastl::vector<LayerInfo> layer_info;
                    layer_info.reserve(canvas.layerInfos.size());
                    for (auto& [_, info] : canvas.layerInfos) {
                        layer_info.push_back(info);
                    }

                    std::ranges::sort(layer_info, [](LayerInfo& a, LayerInfo& b) { return a.height > b.height; });

                    const ImVec2 image_size = {
                        (float)window_size.x / (float)window_size.y * 50.0f,
                        50.0f,
                    };
                    for (int i = static_cast<int>(layer_info.size()) - 1; i >= 0; i--) {
                        auto& real_data = canvas.layerInfos[layer_info[i].id]; // Wut ??
                        const std::string title = std::format("-{}: {}", real_data.height, real_data.name);
                        ImGui::SeparatorText(title.c_str());
                        ImGui::PushID(real_data.id);

                        bool layer_selected = real_data.id == canvas.selectedLayer;
                        if (ImGui::Checkbox("Selected", &layer_selected)) {
                            canvas.selectedLayer = real_data.id;
                        }
                        ImGui::SliderFloat("", &real_data.opacity, 0.0f, 1.0f);

                        // ImGui::Image(
                        //     (ImTextureID)(intptr_t)renderer.layer_textures.at(real_data.layer),
                        //     image_size);

                        if (real_data.height < (canvas.layersCurrentMaxHeight - 1)) {
                            ImGui::SameLine();
                            if (ImGui::Button("-")) {
                                // canvas.canvasHistory.store(std::make_unique<LayerDepthCommand>(
                                // *this, real_data.layer, real_data.depth, real_data.depth + 1));
                                canvas.SetLayerHeight(real_data.id, real_data.height + 1);
                            }
                        }
                        if (real_data.height > 0) {
                            ImGui::SameLine();
                            if (ImGui::Button("+")) {
                                // canvas.canvasHistory.store(std::make_unique<LayerDepthCommand>(
                                // *this, real_data.layer, real_data.depth, real_data.depth - 1));
                                canvas.SetLayerHeight(real_data.id, real_data.height - 1);
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
                            // canvas.canvasHistory.store(std::make_unique<LayerDeleteCommand>(*this, real_data.layer));
                            canvas.DeleteLayer(real_data.id);
                        }

                        ImGui::PopID();
                    }
                    if (ImGui::Button("+")) {
                        LayerInfo layerInfo{};
                        layerInfo.name = "NewLayer";
                        layerInfo.height = 0;
                        const auto newLayer = canvas.CreateLayer(layerInfo);
                        // canvas.canvasHistory.store(std::make_unique<LayerCreateCommand>(*this,
                        // canvas.layerInfos[layer]));
                        canvas.SaveLayer(newLayer);
                    }
                    ImGui::ColorEdit4("Background Color", glm::value_ptr(bg_color));
                }
                ImGui::End();

                if (ImGui::Begin("Stroke Options")) {
                    if (ImGui::Checkbox("Eraser", &canvas.eraserMode)) {
                        canvas.brushMode = !canvas.eraserMode;
                    }

                    if (canvas.eraserMode) {
                        {
                            ImGui::PushID("Opacity");
                            if (ImGui::SliderFloat("Opacity", &canvas.eraserOptions.opacity, 0.0f, 1.0f)) {
                                canvas.eraserOptions.opacity =
                                    std::ceil(canvas.eraserOptions.opacity * 100.0f) / 100.0f;
                                canvas.eraserOptionsModified = true;
                            }
                            ImGui::SameLine();
                            if (ImGui::Checkbox("Pen", &canvas.eraserOptions.opacityPressure)) {
                                canvas.eraserOptionsModified = true;
                            }
                            if (canvas.eraserOptions.opacityPressure) {
                                if (ImGui::SliderFloat2("range",
                                                        glm::value_ptr(canvas.eraserOptions.opacityPressureRange), 0.0f,
                                                        1.0f)) {
                                    canvas.eraserOptions.opacityPressureRange =
                                        glm::ceil(canvas.eraserOptions.opacityPressureRange * 100.0f) / 100.0f;
                                    canvas.eraserOptions.opacityPressureRange.x =
                                        std::min(canvas.eraserOptions.opacityPressureRange.x,
                                                 canvas.eraserOptions.opacityPressureRange.y);
                                    canvas.eraserOptions.opacityPressureRange.y =
                                        std::max(canvas.eraserOptions.opacityPressureRange.x,
                                                 canvas.eraserOptions.opacityPressureRange.y);
                                    canvas.eraserOptionsModified = true;
                                }
                            }
                            ImGui::PopID();
                        }
                        {
                            ImGui::PushID("Radius");
                            if (ImGui::SliderFloat("Radius", &canvas.eraserOptions.radius, 0.0f, 100.0f)) {
                                canvas.eraserOptions.radius = std::ceil(canvas.eraserOptions.radius * 100.0f) / 100.0f;
                                canvas.eraserOptionsModified = true;
                            }
                            ImGui::SameLine();
                            if (ImGui::Checkbox("Pen", &canvas.eraserOptions.radiusPressure)) {
                                canvas.eraserOptionsModified = true;
                            }
                            if (canvas.eraserOptions.radiusPressure) {
                                if (ImGui::SliderFloat2("Radius Range",
                                                        glm::value_ptr(canvas.eraserOptions.radiusPressureRange), 0.0f,
                                                        1.0f)) {
                                    canvas.eraserOptions.radiusPressureRange =
                                        glm::ceil(canvas.eraserOptions.radiusPressureRange * 100.0f) / 100.0f;
                                    canvas.eraserOptions.radiusPressureRange.x =
                                        std::min(canvas.eraserOptions.radiusPressureRange.x,
                                                 canvas.eraserOptions.radiusPressureRange.y);
                                    canvas.eraserOptions.radiusPressureRange.y =
                                        std::max(canvas.eraserOptions.radiusPressureRange.x,
                                                 canvas.eraserOptions.radiusPressureRange.y);
                                    canvas.eraserOptionsModified = true;
                                }
                            }
                            ImGui::PopID();
                        }
                        {
                            ImGui::PushID("Flow");
                            if (ImGui::SliderFloat("Flow", &canvas.eraserOptions.flow, 0.01f, 1.0f)) {
                                canvas.eraserOptions.flow = std::ceil(canvas.eraserOptions.flow * 100.0f) / 100.0f;
                                canvas.eraserOptionsModified = true;
                            }
                            ImGui::SameLine();
                            if (ImGui::Checkbox("Pen", &canvas.eraserOptions.flowPressure)) {
                                canvas.eraserOptionsModified = true;
                            }
                            if (canvas.eraserOptions.flowPressure) {
                                if (ImGui::SliderFloat2("range", glm::value_ptr(canvas.eraserOptions.flowPressureRange),
                                                        0.0f, 1.0f)) {
                                    canvas.eraserOptions.flowPressureRange =
                                        glm::ceil(canvas.eraserOptions.flowPressureRange * 100.0f) / 100.0f;
                                    canvas.eraserOptions.flowPressureRange.x =
                                        std::min(canvas.eraserOptions.flowPressureRange.x,
                                                 canvas.eraserOptions.flowPressureRange.y);
                                    canvas.eraserOptions.flowPressureRange.y =
                                        std::max(canvas.eraserOptions.flowPressureRange.x,
                                                 canvas.eraserOptions.flowPressureRange.y);
                                    canvas.eraserOptionsModified = true;
                                }
                            }
                            ImGui::PopID();
                        }
                        {
                            ImGui::PushID("Hardness");
                            if (ImGui::SliderFloat("Hardness", &canvas.eraserOptions.hardness, 0.0f, 1.0f)) {
                                canvas.eraserOptions.hardness =
                                    std::ceil(canvas.eraserOptions.hardness * 100.0f) / 100.0f;
                                canvas.eraserOptionsModified = true;
                            }
                            ImGui::SameLine();
                            if (ImGui::Checkbox("Pen", &canvas.eraserOptions.hardness_pressure)) {
                                canvas.eraserOptionsModified = true;
                            }
                            if (canvas.eraserOptions.hardness_pressure) {
                                if (ImGui::SliderFloat2("range",
                                                        glm::value_ptr(canvas.eraserOptions.hardnessPressureRange),
                                                        0.0f, 1.0f)) {
                                    canvas.eraserOptions.hardnessPressureRange =
                                        glm::ceil(canvas.eraserOptions.hardnessPressureRange * 100.0f) / 100.0f;
                                    canvas.eraserOptions.hardnessPressureRange.x =
                                        std::min(canvas.eraserOptions.hardnessPressureRange.x,
                                                 canvas.eraserOptions.hardnessPressureRange.y);
                                    canvas.eraserOptions.hardnessPressureRange.y =
                                        std::max(canvas.eraserOptions.hardnessPressureRange.x,
                                                 canvas.eraserOptions.hardnessPressureRange.y);
                                    canvas.eraserOptionsModified = true;
                                }
                            }
                            ImGui::PopID();
                        }
                        if (ImGui::SliderFloat("Spacing", &canvas.eraserOptions.spacing, 0.1f, 50.0f)) {
                            canvas.eraserOptions.spacing = std::ceil(canvas.eraserOptions.spacing * 100.0f) / 100.0f;
                            canvas.eraserOptionsModified = true;
                        }
                        ImGui::Separator();
                        ImGui::LabelText("Stroke num", "%llu", canvas.stroke_points.size());
                        ImGui::Checkbox("Using pen", &pen_in_range);
                        ImGui::LabelText("Cursor", "x: %.2f, y: %.2f", cursor_current_pos.x, cursor_current_pos.y);
                        ImGui::LabelText("Pressure", "%.2f", pen_pressure);
                    } else if (canvas.brushMode) {
                        {
                            ImGui::PushID("Opacity");
                            if (ImGui::ColorEdit4("Color", glm::value_ptr(canvas.brushOptions.color))) {
                                canvas.brushOptionsModified = true;
                            }
                            ImGui::SameLine();
                            if (ImGui::Checkbox("Pen", &canvas.brushOptions.opacityPressure)) {
                                canvas.brushOptionsModified = true;
                            }
                            if (canvas.brushOptions.opacityPressure) {
                                if (ImGui::SliderFloat2("range",
                                                        glm::value_ptr(canvas.brushOptions.opacityPressureRange), 0.0f,
                                                        1.0f)) {
                                    canvas.brushOptions.opacityPressureRange =
                                        glm::ceil(canvas.brushOptions.opacityPressureRange * 100.0f) / 100.0f;
                                    canvas.brushOptions.opacityPressureRange.x =
                                        std::min(canvas.brushOptions.opacityPressureRange.x,
                                                 canvas.brushOptions.opacityPressureRange.y);
                                    canvas.brushOptions.opacityPressureRange.y =
                                        std::max(canvas.brushOptions.opacityPressureRange.x,
                                                 canvas.brushOptions.opacityPressureRange.y);
                                    canvas.brushOptionsModified = true;
                                }
                            }
                            ImGui::PopID();
                        }
                        {
                            ImGui::PushID("Radius");
                            if (ImGui::SliderFloat("Radius", &canvas.brushOptions.radius, 0.0f, 100.0f)) {
                                canvas.brushOptions.radius = std::ceil(canvas.brushOptions.radius * 100.0f) / 100.0f;
                                canvas.brushOptionsModified = true;
                            }
                            ImGui::SameLine();
                            if (ImGui::Checkbox("Pen", &canvas.brushOptions.radiusPressure)) {
                                canvas.brushOptionsModified = true;
                            }
                            if (canvas.brushOptions.radiusPressure) {
                                if (ImGui::SliderFloat2(
                                        "range", glm::value_ptr(canvas.brushOptions.radiusPressureRange), 0.0f, 1.0f)) {
                                    canvas.brushOptions.radiusPressureRange =
                                        glm::ceil(canvas.brushOptions.radiusPressureRange * 100.0f) / 100.0f;
                                    canvas.brushOptions.radiusPressureRange.x =
                                        std::min(canvas.brushOptions.radiusPressureRange.x,
                                                 canvas.brushOptions.radiusPressureRange.y);
                                    canvas.brushOptions.radiusPressureRange.y =
                                        std::max(canvas.brushOptions.radiusPressureRange.x,
                                                 canvas.brushOptions.radiusPressureRange.y);
                                    canvas.brushOptionsModified = true;
                                }
                            }
                            ImGui::PopID();
                        }
                        {
                            ImGui::PushID("Flow");
                            if (ImGui::SliderFloat("Flow", &canvas.brushOptions.flow, 0.01f, 1.0f)) {
                                canvas.brushOptions.flow = std::ceil(canvas.brushOptions.flow * 100.0f) / 100.0f;
                                canvas.brushOptionsModified = true;
                            }
                            ImGui::SameLine();
                            if (ImGui::Checkbox("Pen", &canvas.brushOptions.flowPressure)) {
                                canvas.brushOptionsModified = true;
                            }
                            if (canvas.brushOptions.flowPressure) {
                                if (ImGui::SliderFloat2("range", glm::value_ptr(canvas.brushOptions.flowPressureRange),
                                                        0.0f, 1.0f)) {
                                    canvas.brushOptions.flowPressureRange =
                                        glm::ceil(canvas.brushOptions.flowPressureRange * 100.0f) / 100.0f;
                                    canvas.brushOptions.flowPressureRange.x =
                                        std::min(canvas.brushOptions.flowPressureRange.x,
                                                 canvas.brushOptions.flowPressureRange.y);
                                    canvas.brushOptions.flowPressureRange.y =
                                        std::max(canvas.brushOptions.flowPressureRange.x,
                                                 canvas.brushOptions.flowPressureRange.y);
                                    canvas.brushOptionsModified = true;
                                }
                            }
                            ImGui::PopID();
                        }
                        {
                            ImGui::PushID("Hardness");
                            if (ImGui::SliderFloat("Hardness", &canvas.brushOptions.hardness, 0.0f, 1.0f)) {
                                canvas.brushOptions.hardness =
                                    std::ceil(canvas.brushOptions.hardness * 100.0f) / 100.0f;
                                canvas.brushOptionsModified = true;
                            }
                            ImGui::SameLine();
                            if (ImGui::Checkbox("Pen", &canvas.brushOptions.hardness_pressure)) {
                                canvas.brushOptionsModified = true;
                            }
                            if (canvas.brushOptions.hardness_pressure) {
                                if (ImGui::SliderFloat2("range",
                                                        glm::value_ptr(canvas.brushOptions.hardnessPressureRange), 0.0f,
                                                        1.0f)) {
                                    canvas.brushOptions.hardnessPressureRange =
                                        glm::ceil(canvas.brushOptions.hardnessPressureRange * 100.0f) / 100.0f;
                                    canvas.brushOptions.hardnessPressureRange.x =
                                        std::min(canvas.brushOptions.hardnessPressureRange.x,
                                                 canvas.brushOptions.hardnessPressureRange.y);
                                    canvas.brushOptions.hardnessPressureRange.y =
                                        std::max(canvas.brushOptions.hardnessPressureRange.x,
                                                 canvas.brushOptions.hardnessPressureRange.y);
                                    canvas.brushOptionsModified = true;
                                }
                            }
                            ImGui::PopID();
                        }
                        if (ImGui::SliderFloat("Spacing", &canvas.brushOptions.spacing, 0.1f, 50.0f)) {
                            canvas.brushOptions.spacing = std::ceil(canvas.brushOptions.spacing * 100.0f) / 100.0f;
                            canvas.brushOptionsModified = true;
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
                    if (!canvas.canvasCommands.Empty()) {
                        for (size_t pos = 0; pos < canvas.canvasCommands.Count(); pos++) {
                            const ICommand* command = canvas.canvasCommands.Get(pos);
                            const auto name = command->Name();
                            if (pos < canvas.canvasCommands.currentPosition) {
                                ImGui::TextColored(ImColor(0, 0, 0), "[%llu]: %s", pos, name.c_str());
                            } else if (pos == canvas.canvasCommands.currentPosition) {
                                ImGui::TextColored(ImColor(0, 128, 0), "[%llu]: %s", pos, name.c_str());
                            } else if (pos > canvas.canvasCommands.currentPosition) {
                                ImGui::TextColored(ImColor(0, 128, 128), "[%llu]: %s", pos, name.c_str());
                            }
                        }
                    } else {
                        ImGui::TextColored(ImColor(0, 0, 128), "Nothing");
                    }
                }
                ImGui::End();
                if (ImGui::Begin("View History")) {
                    if (!canvas.viewCommands.Empty()) {
                        for (size_t pos = 0; pos < canvas.viewCommands.Count(); pos++) {
                            const ICommand* command = canvas.viewCommands.Get(pos);
                            const auto name = command->Name();
                            if (pos < canvas.viewCommands.currentPosition) {
                                ImGui::TextColored(ImColor(0, 0, 0), "[%llu]: %s", pos, name.c_str());
                            } else if (pos == canvas.viewCommands.currentPosition) {
                                ImGui::TextColored(ImColor(0, 128, 0), "[%llu]: %s", pos, name.c_str());
                            } else if (pos > canvas.viewCommands.currentPosition) {
                                ImGui::TextColored(ImColor(0, 128, 128), "[%llu]: %s", pos, name.c_str());
                            }
                        }
                    } else {
                        ImGui::TextColored(ImColor(0, 0, 128), "Nothing");
                    }
                }
                ImGui::End();
            }

            if (colorPicker) {
                ImGui::Begin("ColorPicker");
                ImGui::ColorEdit4("ColorPicker", glm::value_ptr(sampledColor), 0);
                ImGui::ColorEdit4("BrushColor", glm::value_ptr(canvas.brushOptions.color), 0);
                ImGui::End();
            }

            if (saving) {
                ImGui::Begin("Saving");
                ImGui::End();
            }
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
        canvas.viewport.Resize(glm::vec2(window_size));
    }

    return true;
}

void App::ShouldQuit() {
    should_quit = true;

    canvas.canvasCommands.Clear();

    const eastl::vector<Layer> layersToSave(canvas.layersModified.begin(), canvas.layersModified.end());
    if (canvas.brushOptionsModified) {
        canvas.SaveBrush();
    }
    if (canvas.eraserOptionsModified) {
        canvas.SaveEraser();
    }

    for (const auto& layer : layersToSave) {
        if (canvas.layerInfos[layer].internal) {
            continue;
        }

        canvas.SaveLayer(layer);
    }

    for (const auto& layer : canvas.Layers()) {
        if (canvas.layerInfos.at(layer).internal) {
            canvas.DeleteLayer(layer);
        }

        for (const auto& tile : canvas.LayerTiles(layer)) {
            canvas.QueueUnloadTile(layer, tile);
        }
    }
}

bool App::CanQuit() {
    return canvas.CanQuit() && renderer.CanQuit();
}

void App::Quit() {
    ZoneScoped;

    SDL_WaitForGPUIdle(renderer.device);

    ImGui_ImplSDL3_Shutdown();
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui::DestroyContext();

    ui.Quit();
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

    if (colorPicker) {
        glm::vec2 delta_pos = new_pos - last_pos;
        last_pos = new_pos;

        Color color;
        auto samplePos = new_pos;
        samplePos.y = window_size.y - new_pos.y;
        canvas.SampleTexture(canvas.canvasTexture_, canvas.canvasTextureSize_, samplePos, color);
        sampledColor.r = static_cast<float>(color.r) / static_cast<float>(UINT8_MAX);
        sampledColor.g = static_cast<float>(color.g) / static_cast<float>(UINT8_MAX);
        sampledColor.b = static_cast<float>(color.b) / static_cast<float>(UINT8_MAX);
        sampledColor.a = static_cast<float>(color.a) / static_cast<float>(UINT8_MAX);
    } else if (cursor_left_pressed) {
        if ((canvas.viewPanning || canvas.viewRotating || canvas.viewZooming) && (glm::length(cursor_delta_pos) > 0) &&
            canvas.currentViewportChangeCommand == nullptr) {
            SDL_assert(canvas.currentViewportChangeCommand == nullptr);
            canvas.currentViewportChangeCommand = std::make_unique<ViewportChangeCommand>(&canvas);
            canvas.currentViewportChangeCommand->SetPreviousViewport(canvas.viewport);
        }

        canvas.ViewUpdateCursor(cursor_current_pos);

        if (canvas.stroke_started) {
            if (canvas.brushMode) {
                canvas.UpdateBrushStroke(Canvas::StrokePoint{
                    .color = canvas.brushOptions.color,
                    .position = canvas.viewport.ScreenToCanvas(cursor_current_pos),
                    .radius = canvas.brushOptions.radius,
                    .flow = canvas.brushOptions.flow,
                    .hardness = canvas.brushOptions.hardness,
                });
            } else if (canvas.eraserMode) {
                canvas.UpdateEraserStroke(Canvas::StrokePoint{
                    .color = {0.0f, 0.0f, 0.0f, canvas.eraserOptions.opacity},
                    .position = canvas.viewport.ScreenToCanvas(cursor_current_pos),
                    .radius = canvas.eraserOptions.radius,
                    .flow = canvas.eraserOptions.flow,
                    .hardness = canvas.eraserOptions.hardness,
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

        if (colorPicker) {
            colorPicker = false;
            canvas.brushOptions.color = sampledColor;
            return;
        }

        if (!canvas.viewPanning && !canvas.viewZooming && !canvas.viewRotating && (canvas.selectedLayer != 0) &&
            !canvas.stroke_started) {
            if (canvas.brushMode) {
                canvas.StartBrushStroke(Canvas::StrokePoint{
                    .color = canvas.brushOptions.color,
                    .position = canvas.viewport.ScreenToCanvas(cursor_current_pos),
                    .radius = canvas.brushOptions.radius,
                    .flow = canvas.brushOptions.flow,
                    .hardness = canvas.brushOptions.hardness,
                });
            } else if (canvas.eraserMode) {
                canvas.StartEraserStroke(Canvas::StrokePoint{
                    .color = {0.0f, 0.0f, 0.0f, canvas.eraserOptions.opacity},
                    .position = canvas.viewport.ScreenToCanvas(cursor_current_pos),
                    .radius = canvas.eraserOptions.radius,
                    .flow = canvas.eraserOptions.flow,
                    .hardness = canvas.eraserOptions.hardness,
                });
            }
        }
    }
}
void App::CursorRelease(Uint8 button) {
    if (button == SDL_BUTTON_LEFT) {
        cursor_left_pressed = false;
        if (!canvas.viewPanning && !canvas.viewZooming && !canvas.viewRotating && (canvas.selectedLayer != 0) &&
            canvas.stroke_started) {
            if (canvas.brushMode) {
                canvas.EndBrushStroke(Canvas::StrokePoint{
                    .color = canvas.brushOptions.color,
                    .position = canvas.viewport.ScreenToCanvas(cursor_current_pos),
                    .radius = canvas.brushOptions.radius,
                    .flow = canvas.brushOptions.flow,
                    .hardness = canvas.brushOptions.hardness,
                });
            } else if (canvas.eraserMode) {
                canvas.EndEraserStroke(Canvas::StrokePoint{
                    .color = {0.0f, 0.0f, 0.0f, canvas.eraserOptions.opacity},
                    .position = canvas.viewport.ScreenToCanvas(cursor_current_pos),
                    .radius = canvas.eraserOptions.radius,
                    .flow = canvas.eraserOptions.flow,
                    .hardness = canvas.eraserOptions.hardness,
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
    (void)mods;
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
    if (key == SDLK_TAB) {
        hide_ui = !hide_ui;
    }
    if (key == SDLK_F && space_pressed) {
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
        if (key == SDLK_C && !ctrl_pressed && !shift_pressed && !alt_pressed) {
            // TODO: Open color wheel at mouse location
        }
        if (alt_pressed && canvas.brushMode) {
            colorPicker = true;
            canvas.canvasTexture_ = canvas.DownloadCanvasTexture(canvas.canvasTextureSize_);
            Color color;
            auto samplePos = cursor_current_pos;
            samplePos.y = window_size.y - cursor_current_pos.y;
            canvas.SampleTexture(canvas.canvasTexture_, canvas.canvasTextureSize_, samplePos, color);
            sampledColor.r = static_cast<float>(color.r) / static_cast<float>(UINT8_MAX);
            sampledColor.g = static_cast<float>(color.g) / static_cast<float>(UINT8_MAX);
            sampledColor.b = static_cast<float>(color.b) / static_cast<float>(UINT8_MAX);
            sampledColor.a = static_cast<float>(color.a) / static_cast<float>(UINT8_MAX);
        }

        if (key == SDLK_B && canvas.eraserMode) {
            canvas.eraserMode = false;
            canvas.brushMode = true;
        }
        if (key == SDLK_E && canvas.brushMode) {
            canvas.brushMode = false;
            canvas.eraserMode = true;
        }

        if (key == SDLK_Z && ctrl_pressed) {
            if (shift_pressed) {
                canvas.canvasCommands.Redo();
            } else {
                canvas.canvasCommands.Undo();
            }
        }

        if (key == SDLK_LEFT && alt_pressed) {
            canvas.viewCommands.Undo();
        }
        if (key == SDLK_RIGHT && alt_pressed) {
            canvas.viewCommands.Redo();
        }

        if (key == SDLK_S && ctrl_pressed && !shift_pressed && !alt_pressed) {
            Save();
        }
    }

    canvas.ViewUpdateState(cursor_current_pos);
}

void App::Save() {
    if (saving) {
        return;
    }
    saving = true;

    const eastl::vector<Layer> layersToSave(canvas.layersModified.begin(), canvas.layersModified.end());
    if (canvas.brushOptionsModified) {
        canvas.SaveBrush();
    }
    if (canvas.eraserOptionsModified) {
        canvas.SaveEraser();
    }

    for (const auto& layer : layersToSave) {
        if (canvas.layerInfos[layer].internal) {
            continue;
        }

        canvas.SaveLayer(layer);
    }

    for (const auto& layer : canvas.Layers()) {
        for (const auto& tile : canvas.LayerTiles(layer)) {
            canvas.QueueSaveTile(layer, tile);
        }
    }

    saving = false;
}

void App::KeyRelease(SDL_Keycode key, SDL_Keymod mods) {
    (void)mods;
    if (key == SDLK_SPACE) {
        space_pressed = false;
        if (canvas.currentViewportChangeCommand != nullptr) {
            canvas.currentViewportChangeCommand->SetNewViewport(canvas.viewport);
            canvas.viewCommands.Push(std::move(canvas.currentViewportChangeCommand));
            SDL_assert(canvas.currentViewportChangeCommand == nullptr);
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

    if (colorPicker && !alt_pressed) {
        SDL_assert(canvas.brushMode);
        colorPicker = false;
        canvas.canvasTexture_.clear();
    }

    if (changeCursorSize && key == SDLK_F) {
        changeCursorSize = false;
        return;
    }

    canvas.ViewUpdateState(cursor_current_pos);
}

void App::DebugTileCulling(glm::vec2 viewportSize, ImDrawList* drawList) {
    const auto tilePositions = canvas.viewport.VisibleTiles();
    for(const auto& tilePosition : tilePositions) {
        DrawTileDebug(tilePosition, drawList, IM_COL32(255, 0, 0, 128));
    }
}

void App::DrawTileDebug(glm::ivec2 pos, ImDrawList* drawList, ImU32 col, const std::string& label) {
    ImVec2 points[5]{};
    ImVec2 center;
    glm::vec2 offsets[5] = {
        {1.0f, 1.0f},
        {static_cast<float>(TILE_WIDTH), 0.0f},
        {static_cast<float>(TILE_HEIGHT), static_cast<float>(TILE_WIDTH)},
        {1.0f, static_cast<float>(TILE_HEIGHT)},
        {1.0f, 1.0f},
    };
    for (size_t i = 0; i < 5; i++) {
        glm::vec4 p = canvas.viewport.ViewMatrix() *
                      glm::vec4(static_cast<glm::vec2>(pos) * static_cast<float>(TILE_WIDTH, TILE_HEIGHT) + offsets[i],
                                0.0f, 1.0f);

        points[i] = ImVec2{
            static_cast<float>(window_size.x) / 2.0f + p.x,
            static_cast<float>(window_size.y) / 2.0f + p.y,
        };
        center.x += points[i].x / 5.0f;
        center.y += points[i].y / 5.0f;
    }

    const auto text = std::format("[{},{}]", pos.x, pos.y);
    drawList->AddText(ImVec2(center.x, center.y), col, text.c_str());
    if(!label.empty()) {
        drawList->AddText(ImVec2(center.x, center.y + 20.0f), col, label.c_str());
    }
    drawList->AddPolyline(points, 5, col, 0, 1.0f);
}

void App::Fullscreen(bool enable) {
    SDL_SetWindowFullscreen(window, enable);
    fullscreen = enable;
}

} // namespace Midori