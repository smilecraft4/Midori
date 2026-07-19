#include "app.hpp"
#include "states.hpp"

#include <SDL3/SDL_log.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlgpu3.h>
#include <format>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <tracy/Tracy.hpp>

namespace Midori {

App::App(int argc, char* argv[]) : renderer(this), canvas(this), stateManager(this) {
    ZoneScoped;
    args.resize(argc);
    for (size_t i = 0; i < args.size(); i++) {
        args[i] = std::string(argv[i]);
    }
}

bool App::OnEvent(const SDL_Event* event) {
    stateManager.OnEvent(event);

    return true;
}

bool App::Init() {
    ZoneScoped;

    float mainScale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (mainScale == 0.0F) {
        mainScale = 1.0F;
    }

    window = SDL_CreateWindow("Midori", windowSize.x, windowSize.y,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to create window: %s", SDL_GetError());
        return false;
    }
    SDL_SetWindowPosition(window, windowPos.x, windowPos.y);
    SDL_ShowWindow(window);

    SDL_GetMouseState(&cursorCurrentPos.x, &cursorCurrentPos.y);
    cursorLastPos = cursorCurrentPos;

    if (!renderer.Init()) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize renderer");
        return false;
    }

    canvas.viewport.Resize(glm::vec2(windowSize));
    if (!canvas.Open()) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to create new canvas");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
#if defined(MIDORI_WINDOWS)
    io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/Segoeui.ttf", 16.0f);
#elif defined(MIDORI_LINUX)
    io.Fonts->AddFontFromFileTTF("/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf", 16.0f);
#endif

    ImGui::StyleColorsLight();

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(mainScale);
    style.FontScaleDpi = mainScale;
    // TODO: load cascadia font

    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo initInfo = {
        .Device = renderer.device,
        .ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(renderer.device, window),
        .MSAASamples = SDL_GPU_SAMPLECOUNT_1,
        .SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
        .PresentMode = SDL_GPU_PRESENTMODE_VSYNC,
    };
    ImGui_ImplSDLGPU3_Init(&initInfo);

    return true;
}

static const SDL_DialogFileFilter FILTERS[] = {
    {.name = "Midori files (.mido)", .pattern = "mido"},
    {.name = "All files", .pattern = "*"},
};

void App::Update() {
    // std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const size_t frameArenaUsage = frameArena.size();
    const size_t strokeArenaUsage = canvas.strokeArena.size();
    frameArena.reset();

    if (windowSize.x > 0 && windowSize.y > 0 && !hidden) {
        { // ImGui Stuff
            ZoneScopedN("UI");
            ImGui_ImplSDLGPU3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            static bool uiDebugCulling = true;
            auto layer = canvas.selectedLayer;
            if (canvas.strokeLayer > 0) {
                layer = canvas.strokeLayer;
            }

            ImDrawList* drawList = ImGui::GetBackgroundDrawList();
            if (uiDebugCulling && layer > 0) {
                for (const auto& tile : canvas.tilesLoaded) {
                    if (canvas.tilesModified.contains(tile)) {
                        ImU32 col = IM_COL32(0, 0, 255, 64);
                        const char* str{};
                        DrawTileDebug({tile.x, tile.y}, drawList, col, "Modified");
                    } else if (canvas.tilesReading.contains(tile)) {
                        ImU32 col = IM_COL32(255, 0, 0, 64);
                        const char* str{};
                        DrawTileDebug({tile.x, tile.y}, drawList, col, "Reading");
                    } else if (canvas.tilesWriting.contains(tile)) {
                        ImU32 col = IM_COL32(0, 255, 0, 64);
                        const char* str{};
                        DrawTileDebug({tile.x, tile.y}, drawList, col, "Writing");
                    } else {
                        ImU32 col = IM_COL32(0, 0, 0, 64);
                        DrawTileDebug({tile.x, tile.y}, drawList, col);
                    }
                }
            }
            // DebugTileCulling(glm::vec2(windowSize) / 2.0f, drawList);

            if (ImGui::Begin("Debug")) {
                ImGui::SeparatorText("Memory");
                ImGui::Text("Frame: %.03f/%.03fKB", static_cast<float>(frameArenaUsage) / 1_KB,
                            static_cast<float>(frameArena.capacity()) / 1_KB);
                ImGui::Text("Stroke: %.03f/%.03fKB", static_cast<float>(strokeArenaUsage) / 1_KB,
                            static_cast<float>(canvas.strokeArena.capacity()) / 1_KB);
                ImGui::SeparatorText("IO");
                ImGui::SeparatorText("State");
                stateManager.DrawUI();
            }
            ImGui::End();

            if (!hideUi) {
                canvas.viewport.UI();

                if (ImGui::Begin("Debug")) {
                    ImGui::Checkbox("view loaded tiles", &uiDebugCulling);
                    ImGui::LabelText("tile loaded", "%zu", canvas.tilesLoaded.size());
                    ImGui::LabelText("tile textures", "%zu", renderer.tileTextures.size());
                    ImGui::LabelText("tile modified", "%zu", canvas.tilesModified.size());
                }
                ImGui::End();

                if (ImGui::Begin("Layers")) {
                    const ImVec2 imageSize = {
                        (float)windowSize.x / (float)windowSize.y * 50.0f,
                        50.0f,
                    };
                    for (int i = static_cast<int>(canvas.layersTree.size()) - 1; i >= 0; i--) {
                        auto& realData = canvas.layers.at(canvas.layersTree[i]);

                        const std::string title = std::format("-{}: {}", realData.height, realData.name);
                        ImGui::SeparatorText(title.c_str());
                        ImGui::PushID(realData.id);

                        bool layerSelected = realData.id == canvas.selectedLayer;
                        if (ImGui::Checkbox("Selected", &layerSelected)) {
                            canvas.selectedLayer = realData.id;
                        }
                        ImGui::SliderFloat("", &realData.opacity, 0.0f, 1.0f);

                        if (i < (canvas.layersTree.size() - 1)) {
                            ImGui::SameLine();
                            if (ImGui::Button("-")) {
                                canvas.MoveLayer(realData.id, realData.height + 1); // will break
                            }
                        }
                        if (realData.height > 0) {
                            ImGui::SameLine();
                            if (ImGui::Button("+")) {
                                canvas.MoveLayer(realData.id, realData.height - 1); // will break
                            }
                        }
                        if (i != 0) {
                            ImGui::SameLine();
                            if (ImGui::Button("=")) {
                                auto srcLayer = canvas.layersTree[i];
                                auto dstLayer = canvas.layersTree[i - 1];
                                canvas.MergeLayers(srcLayer, dstLayer);
                                canvas.DeleteLayer(srcLayer); // will break
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("x")) {
                            canvas.DeleteLayer(realData.id);
                        }

                        ImGui::PopID();
                    }
                    if (ImGui::Button("+")) {
                        LayerCreateInfo layerInfo{};
                        layerInfo.name = "NewLayer";
                        layerInfo.height = 0;
                        const auto newLayer = canvas.CreateLayer(layerInfo);
                        canvas.SaveLayer(newLayer);
                    }
                    ImGui::ColorEdit4("Background Color", glm::value_ptr(bgColor));
                }
                ImGui::End();

                if (ImGui::Begin("Stroke Options")) {
                    bool painting = canvas.currentTool == Tool::Paint;
                    if (ImGui::Checkbox("Paint", &painting)) {
                        canvas.currentTool = Tool::Paint;
                    }
                    bool erasing = canvas.currentTool == Tool::Erase;
                    if (ImGui::Checkbox("Erase", &erasing)) {
                        canvas.currentTool = Tool::Erase;
                    }

                    switch (canvas.currentTool) {
                        case Tool::Paint: {
                            ImGui::Text("Painting");
                            if (ImGui::ColorEdit4("Color", glm::value_ptr(canvas.paintBrush.color))) {
                                canvas.paintBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Min Opacity", &canvas.paintBrush.minPressureOpacity, 0.0f, 1.0f)) {
                                canvas.paintBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Max Opacity", &canvas.paintBrush.maxPressureOpacity, 0.0f, 1.0f)) {
                                canvas.paintBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Min Radius", &canvas.paintBrush.minPressureRadius, 0.0f, 25.0f)) {
                                canvas.paintBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Max Radius", &canvas.paintBrush.maxPressureRadius, 0.0f, 25.0f)) {
                                canvas.paintBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Min Flow", &canvas.paintBrush.minPressureFlow, 0.0f, 1.0f)) {
                                canvas.paintBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Max Flow", &canvas.paintBrush.maxPressureFlow, 0.0f, 1.0f)) {
                                canvas.paintBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Min Hardness", &canvas.paintBrush.minPressureHardness, 0.0f,
                                                   1.0f)) {
                                canvas.paintBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Max Hardness", &canvas.paintBrush.maxPressureHardness, 0.0f,
                                                   1.0f)) {
                                canvas.paintBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Min Spacing", &canvas.paintBrush.minPressureSpacing, 0.1f, 50.0f)) {
                                canvas.paintBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Max Spacing", &canvas.paintBrush.maxPressureSpacing, 0.1f, 50.0f)) {
                                canvas.paintBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Smoothing", &canvas.paintBrush.smoothing, 0.0f, 1.0f)) {
                                canvas.paintBrushModified = true;
                            }
                        } break;
                        case Tool::Erase: {
                            ImGui::Text("Erasing");
                            if (ImGui::SliderFloat("Min Opacity", &canvas.eraseBrush.minPressureOpacity, 0.0f, 1.0f)) {
                                canvas.eraseBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Max Opacity", &canvas.eraseBrush.maxPressureOpacity, 0.0f, 1.0f)) {
                                canvas.eraseBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Min Radius", &canvas.eraseBrush.minPressureRadius, 0.0f, 25.0f)) {
                                canvas.eraseBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Max Radius", &canvas.eraseBrush.maxPressureRadius, 0.0f, 25.0f)) {
                                canvas.eraseBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Min Flow", &canvas.eraseBrush.minPressureFlow, 0.0f, 1.0f)) {
                                canvas.eraseBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Max Flow", &canvas.eraseBrush.maxPressureFlow, 0.0f, 1.0f)) {
                                canvas.eraseBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Min Hardness", &canvas.eraseBrush.minPressureHardness, 0.0f,
                                                   1.0f)) {
                                canvas.eraseBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Max Hardness", &canvas.eraseBrush.maxPressureHardness, 0.0f,
                                                   1.0f)) {
                                canvas.eraseBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Min Spacing", &canvas.eraseBrush.minPressureSpacing, 0.1f, 50.0f)) {
                                canvas.eraseBrushModified = true;
                            }
                            if (ImGui::SliderFloat("Max Spacing", &canvas.eraseBrush.maxPressureSpacing, 0.1f, 50.0f)) {
                                canvas.eraseBrushModified = true;
                            }
                        } break;
                        default: {
                            ImGui::Text("Unknown");
                        } break;
                    }
                }
                ImGui::End();

                if (ImGui::Begin("Canvas History")) {
                    canvas.canvasCommands.DrawUI();
                }
                ImGui::End();

                if (ImGui::Begin("View History")) {
                    canvas.viewCommands.DrawUI();
                }
                ImGui::End();
            }

            if (colorPicker) {
                ImGui::Begin("ColorPicker");
                ImGui::ColorEdit4("ColorPicker", glm::value_ptr(sampledColor), 0);
                ImGui::ColorEdit4("BrushColor", glm::value_ptr(canvas.paintBrush.color), 0);
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
    renderer.Update();
    renderer.Render();
}

bool App::Resize(const int width, const int height) {
    ZoneScoped;
    windowSize.x = width;
    windowSize.y = height;

    if (windowSize.x > 0 || windowSize.y > 0) {
        if (!renderer.Resize()) {
            return false;
        }
        canvas.viewport.Resize(glm::vec2(windowSize));
    }

    return true;
}

void App::ShouldQuit() {
    shouldQuit = true;

    canvas.canvasCommands.Clear();

    for (const auto& [id, layer] : canvas.layers) {
        if (layer.modified && !layer.internal) {
            canvas.SaveLayer(id);
        }
    }

    for (const auto& tile : canvas.tilesLoaded) {
        canvas.UnloadTile(tile);
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

    renderer.Quit();

    SDL_DestroyWindow(window);
    window = nullptr;
}

void App::Save() {
    if (saving) {
        return;
    }
    saving = true;

    if (canvas.eraseBrushModified) {
        canvas.SaveEraseBrush();
    }

    if (canvas.paintBrushModified) {
        canvas.SavePaintBrush();
    }

    for (const auto& [id, layer] : canvas.layers) {
        if (layer.modified && !layer.internal) {
            canvas.SaveLayer(id);
        }
    }

    // FIXME Use a tmp vector/iterator to avoid invalidating ptrs
    for (const auto& tile : canvas.tilesModified) {
        if (!canvas.tilesLoaded.contains(tile)) {
            canvas.DeleteTile(tile);
        } else {
            canvas.SaveTile(tile);
        }
    }

    saving = false;
}

void App::DebugTileCulling(glm::vec2 viewportSize, ImDrawList* drawList) {
    const auto tilePositions = canvas.viewport.VisibleTiles(frameArena);
    for (const auto& tilePosition : tilePositions) {
        DrawTileDebug(tilePosition, drawList, IM_COL32(255, 0, 0, 128));
    }
}

void App::DrawTileDebug(glm::ivec2 pos, ImDrawList* drawList, ImU32 col, const std::string& label) const {
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
        glm::vec4 p =
            canvas.viewport.ViewMatrix()
            * glm::vec4(static_cast<glm::vec2>(pos) * glm::vec2(TILE_WIDTH, TILE_HEIGHT) + offsets[i], 0.0f, 1.0f);

        points[i] = ImVec2{
            std::floor((static_cast<float>(windowSize.x) / 2.0f) + p.x),
            std::floor((static_cast<float>(windowSize.y) / 2.0f) + p.y),
        };
        center.x += std::floor(points[i].x / 5.0f);
        center.y += std::floor(points[i].y / 5.0f);
    }

    const auto text = std::format("[{},{}]", pos.x, pos.y);
    drawList->AddText(ImVec2(center.x, center.y), col, text.c_str());
    if (!label.empty()) {
        drawList->AddText(ImVec2(center.x, center.y + 20.0f), col, label.c_str());
    }
    drawList->AddPolyline(points, 5, col, 0, 1.0f);
}

void App::Fullscreen(bool enable) {
    SDL_SetWindowFullscreen(window, enable);
    fullscreen = enable;
}

} // namespace Midori
