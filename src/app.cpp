#include "app.hpp"
#include "states.hpp"

#include <SDL3/SDL_log.h>
#include "../deps/json.hpp"
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlgpu3.h>
#include <filesystem>
#include <format>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <tracy/Tracy.hpp>

namespace Midori {

App::App(int argc, char* argv[])
    : renderer(this), stateManager(this), workers(std::thread::hardware_concurrency() - 2, "Worker") {
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

    const auto canvasFile = canvasFiles.emplace(this);

#ifdef NDEBUG
    const std::string path = SDL_GetPrefPath(nullptr, "midori");
#else
    const std::string path = SDL_GetPrefPath(nullptr, "midori-dev");
#endif
    auto filename = std::format("{}file.mido", path);
    if (std::filesystem::exists(filename)) {
        canvasFiles.at(canvasFile).Open(filename);
    } else {
        canvasFiles.at(canvasFile).New();
        canvasFiles.at(canvasFile).SaveAs(filename);
    }

    currentCanvasView = canvasViews.emplace(this, 0, canvasFile);
    canvasViews.at(currentCanvasView.value()).Init();
    canvasViews.at(currentCanvasView.value()).Resize(windowSize.x, windowSize.y);

    return true;
}

static const SDL_DialogFileFilter FILTERS[] = {
    {.name = "Midori files (.mido)", .pattern = "mido"},
    {.name = "All files", .pattern = "*"},
};

void App::Update() {
    auto& canvasView = canvasViews.at(currentCanvasView.value());
    auto& canvasFile = canvasFiles.at(canvasView.canvasFileId);

    const size_t frameArenaUsage = frameArena.size();
    const size_t strokeArenaUsage = strokeArena.size();

    frameArena.reset();

    if (windowSize.x > 0 && windowSize.y > 0 && !hidden) {
        { // ImGui Stuff
            ZoneScopedN("UI");
            ImGui_ImplSDLGPU3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            auto& canvasView = canvasViews.at(currentCanvasView.value());

            static bool uiDebugCulling = true;
            auto layer = canvasView.selectedLayer;
            if (uiDebugCulling && layer.has_value()) {
                ImDrawList* drawList = ImGui::GetBackgroundDrawList();
                for (const auto& tile : canvasFile.tilesLoaded) {
                    if (canvasFile.tilesModified.contains(tile)) {
                        ImU32 col = IM_COL32(0, 0, 255, 64);
                        const char* str{};
                        DrawTileDebug({tile.x, tile.y}, drawList, col, "Modified");
                    } else if (canvasFile.tilesReading.contains(tile)) {
                        ImU32 col = IM_COL32(255, 0, 0, 64);
                        const char* str{};
                        DrawTileDebug({tile.x, tile.y}, drawList, col, "Reading");
                    } else if (canvasFile.tilesWriting.contains(tile)) {
                        ImU32 col = IM_COL32(0, 255, 0, 64);
                        const char* str{};
                        DrawTileDebug({tile.x, tile.y}, drawList, col, "Writing");
                    } else {
                        ImU32 col = IM_COL32(0, 0, 0, 64);
                        DrawTileDebug({tile.x, tile.y}, drawList, col);
                    }
                }
            }

            if (ImGui::Begin("Debug")) {
                ImGui::SeparatorText("Memory");
                ImGui::Text("Frame: %.03f/%.03fKB", static_cast<float>(frameArenaUsage) / 1_KB,
                            static_cast<float>(frameArena.capacity()) / 1_KB);
                ImGui::Text("Stroke: %.03f/%.03fKB", static_cast<float>(strokeArenaUsage) / 1_KB,
                            static_cast<float>(strokeArena.capacity()) / 1_KB);
                ImGui::SeparatorText("IO");
                ImGui::SeparatorText("State");
                stateManager.DrawUI();
            }
            ImGui::End();

            if (!hideUi) {
                if (ImGui::Begin("Debug")) {
                    ImGui::Checkbox("view loaded tiles", &uiDebugCulling);
                    ImGui::LabelText("tile loaded", "%zu", canvasFile.tilesLoaded.size());
                    ImGui::LabelText("tile textures", "%zu", renderer.tileTextures.size());
                    ImGui::LabelText("tile modified", "%zu", canvasFile.tilesModified.size());
                }
                ImGui::End();

                if (ImGui::Begin("Layers")) {
                    const ImVec2 imageSize = {
                        (float)windowSize.x / (float)windowSize.y * 50.0f,
                        50.0f,
                    };
                    for (int i = static_cast<int>(canvasFile.layersTree.size()) - 1; i >= 0; i--) {
                        auto& realData = canvasFile.layers.at(canvasFile.layersTree[i]);

                        const std::string title = std::format("-{}: {}", realData.height, realData.name);
                        ImGui::SeparatorText(title.c_str());
                        ImGui::PushID(realData.id);

                        bool layerSelected = realData.id == canvasView.selectedLayer;
                        if (ImGui::Checkbox("Selected", &layerSelected)) {
                            canvasView.selectedLayer = realData.id;
                        }
                        ImGui::SliderFloat("", &realData.opacity, 0.0f, 1.0f);

                        if (i < (canvasFile.layersTree.size() - 1)) {
                            ImGui::SameLine();
                            if (ImGui::Button("-")) {
                                canvasFile.SetLayerHeight(realData.id, realData.height + 1); // will break
                            }
                        }
                        if (realData.height > 0) {
                            ImGui::SameLine();
                            if (ImGui::Button("+")) {
                                canvasFile.SetLayerHeight(realData.id, realData.height - 1); // will break
                            }
                        }
                        if (i != 0) {
                            ImGui::SameLine();
                            if (ImGui::Button("=")) {
                                auto srcLayer = canvasFile.layersTree[i];
                                auto dstLayer = canvasFile.layersTree[i - 1];
                                canvasFile.MergeLayers(srcLayer, dstLayer);
                                canvasFile.DeleteLayer(srcLayer); // will break
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("x")) {
                            canvasFile.DeleteLayer(realData.id);
                        }

                        ImGui::PopID();
                    }
                    if (ImGui::Button("+")) {
                        LayerCreateInfo layerInfo{};
                        layerInfo.name = "NewLayer";
                        layerInfo.height = 0;
                        const auto newLayer = canvasFile.NewLayer(layerInfo);
                    }
                    ImGui::ColorEdit4("Background Color", glm::value_ptr(bgColor));
                }
                ImGui::End();

                if (ImGui::Begin("Stroke Options")) {
                    bool painting = currentTool == Tool::Paint;
                    if (ImGui::Checkbox("Paint", &painting)) {
                        currentTool = Tool::Paint;
                    }
                    bool erasing = currentTool == Tool::Erase;
                    if (ImGui::Checkbox("Erase", &erasing)) {
                        currentTool = Tool::Erase;
                    }

                    switch (currentTool) {
                        case Tool::Paint: {
                            ImGui::Text("Painting");
                            if (ImGui::ColorEdit4("Color", glm::value_ptr(paintBrush.color))) {
                                paintBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Min Opacity", &paintBrush.minPressureOpacity, 0.0f, 1.0f)) {
                                paintBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Max Opacity", &paintBrush.maxPressureOpacity, 0.0f, 1.0f)) {
                                paintBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Min Radius", &paintBrush.minPressureRadius, 0.0f, 25.0f)) {
                                paintBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Max Radius", &paintBrush.maxPressureRadius, 0.0f, 25.0f)) {
                                paintBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Min Flow", &paintBrush.minPressureFlow, 0.0f, 1.0f)) {
                                paintBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Max Flow", &paintBrush.maxPressureFlow, 0.0f, 1.0f)) {
                                paintBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Min Hardness", &paintBrush.minPressureHardness, 0.0f, 1.0f)) {
                                paintBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Max Hardness", &paintBrush.maxPressureHardness, 0.0f, 1.0f)) {
                                paintBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Min Spacing", &paintBrush.minPressureSpacing, 0.1f, 50.0f)) {
                                paintBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Max Spacing", &paintBrush.maxPressureSpacing, 0.1f, 50.0f)) {
                                paintBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Smoothing", &paintBrush.smoothing, 0.0f, 1.0f)) {
                                paintBrush.modified = true;
                            }
                        } break;
                        case Tool::Erase: {
                            ImGui::Text("Erasing");
                            if (ImGui::SliderFloat("Min Opacity", &eraseBrush.minPressureOpacity, 0.0f, 1.0f)) {
                                eraseBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Max Opacity", &eraseBrush.maxPressureOpacity, 0.0f, 1.0f)) {
                                eraseBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Min Radius", &eraseBrush.minPressureRadius, 0.0f, 25.0f)) {
                                eraseBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Max Radius", &eraseBrush.maxPressureRadius, 0.0f, 25.0f)) {
                                eraseBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Min Flow", &eraseBrush.minPressureFlow, 0.0f, 1.0f)) {
                                eraseBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Max Flow", &eraseBrush.maxPressureFlow, 0.0f, 1.0f)) {
                                eraseBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Min Hardness", &eraseBrush.minPressureHardness, 0.0f, 1.0f)) {
                                eraseBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Max Hardness", &eraseBrush.maxPressureHardness, 0.0f, 1.0f)) {
                                eraseBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Min Spacing", &eraseBrush.minPressureSpacing, 0.1f, 50.0f)) {
                                eraseBrush.modified = true;
                            }
                            if (ImGui::SliderFloat("Max Spacing", &eraseBrush.maxPressureSpacing, 0.1f, 50.0f)) {
                                eraseBrush.modified = true;
                            }
                        } break;
                        default: {
                            ImGui::Text("Unknown");
                        } break;
                    }
                }
                ImGui::End();

                if (ImGui::Begin("Canvas History")) {
                    canvasFile.canvasHistory.DrawUI();
                }
                ImGui::End();

                if (ImGui::Begin("View History")) {
                    canvasView.viewHistory.DrawUI();
                }
                ImGui::End();
            }

            if (colorPicker) {
                ImGui::Begin("ColorPicker");
                ImGui::ColorEdit4("ColorPicker", glm::value_ptr(sampledColor), 0);
                ImGui::ColorEdit4("BrushColor", glm::value_ptr(paintBrush.color), 0);
                ImGui::End();
            }

            if (saving) {
                ImGui::Begin("Saving");
                ImGui::End();
            }
        }

        ImGui::Render();
    }

    for (auto [id, canvasFile] : canvasFiles) {
        canvasFile.Update();
    }

    for (auto [id, canvasView] : canvasViews) {
        canvasView.Update();
    }

    // canvas.Update();
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

        for (auto [id, canvasView] : canvasViews) {
            canvasView.Update();
        }
        // canvas.viewport.Resize(glm::vec2(windowSize));
    }

    return true;
}

void App::ShouldQuit() {
    shouldQuit = true;

    for (auto [id, canvasFile] : canvasFiles) {
        canvasFile.canvasHistory.Clear();
        canvasFile.Save();
    }

    // canvas.canvasCommands.Clear();
    // for (const auto& [id, layer] : canvas.layers) {
    //     if (layer.modified && !layer.internal) {
    //         canvas.SaveLayer(id);
    //     }
    // }
    //
    // for (const auto& tile : canvas.tilesLoaded) {
    //     canvas.UnloadTile(tile);
    // }
}

bool App::CanQuit() {
    ZoneScoped;

    for (auto [id, canvasFile] : canvasFiles) {
        if (!canvasFile.tilesWriting.empty()) {
            return false;
        }
    }

    if (!renderer.CanQuit()) {
        return false;
    }

    return true;
    // return canvas.CanQuit() && renderer.CanQuit();
}

void App::Quit() {
    ZoneScoped;

    for (auto [id, canvasView] : canvasViews) {
        canvasView.Quit();
    }

    for (auto [id, canvasFile] : canvasFiles) {
        canvasFile.Quit();
    }

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

    if (eraseBrush.modified || paintBrush.modified) {
        SaveBrushes();
    }

    for (auto [id, canvasFile] : canvasFiles) {
        canvasFile.Save();
    }

    // for (const auto& [id, layer] : canvas.layers) {
    //     if (layer.modified && !layer.internal) {
    //         canvas.SaveLayer(id);
    //     }
    // }

    // // FIXME Use a tmp vector/iterator to avoid invalidating ptrs
    // for (const auto& tile : canvas.tilesModified) {
    //     if (!canvas.tilesLoaded.contains(tile)) {
    //         canvas.DeleteTile(tile);
    //     } else {
    //         canvas.SaveTile(tile);
    //     }
    // }

    saving = false;
}
void App::Fullscreen(bool enable) {
    SDL_SetWindowFullscreen(window, enable);
    fullscreen = enable;
}

void App::OpenBrushes() {
    ZoneScoped;
#ifdef NDEBUG
    const std::string path = SDL_GetPrefPath(nullptr, "midori");
#else
    const std::string path = SDL_GetPrefPath(nullptr, "midori-dev");
#endif

    {
        const std::string file = std::format("{}brushes/paintBrush.json", path);

        size_t size;
        char* buf = (char*)SDL_LoadFile(file.c_str(), &size);
        if (size == 0) {
            paintBrush.modified = true;
            return;
        } else {
            paintBrush.modified = false;
        }

        auto brushJson = nlohmann::json::parse(buf);
        SDL_free(buf);

        paintBrush.color.r = brushJson.at("color")[0];
        paintBrush.color.g = brushJson.at("color")[1];
        paintBrush.color.b = brushJson.at("color")[2];
        paintBrush.color.a = brushJson.at("color")[3];
        paintBrush.minPressureOpacity = brushJson.at("minOpacity");
        paintBrush.maxPressureOpacity = brushJson.at("maxOpacity");
        paintBrush.minPressureFlow = brushJson.at("minFlow");
        paintBrush.maxPressureFlow = brushJson.at("maxFlow");
        paintBrush.minPressureRadius = brushJson.at("minRadius");
        paintBrush.maxPressureRadius = brushJson.at("maxRadius");
        paintBrush.minPressureHardness = brushJson.at("minHardness");
        paintBrush.maxPressureHardness = brushJson.at("maxHardness");
        paintBrush.minPressureSpacing = brushJson.at("minSpacing");
        paintBrush.maxPressureSpacing = brushJson.at("maxSpacing");
        paintBrush.smoothing = brushJson.at("smoothing");
    }
    {
        const std::string file = std::format("{}brushes/eraseBrush.json", path);

        size_t size;
        char* buf = (char*)SDL_LoadFile(file.c_str(), &size);
        if (size == 0) {
            eraseBrush.modified = true;
            return;
        } else {
            eraseBrush.modified = false;
        }

        auto brushJson = nlohmann::json::parse(buf);
        SDL_free(buf);

        eraseBrush.color.r = brushJson.at("color")[0];
        eraseBrush.color.g = brushJson.at("color")[1];
        eraseBrush.color.b = brushJson.at("color")[2];
        eraseBrush.color.a = brushJson.at("color")[3];
        eraseBrush.minPressureOpacity = brushJson.at("minOpacity");
        eraseBrush.maxPressureOpacity = brushJson.at("maxOpacity");
        eraseBrush.minPressureFlow = brushJson.at("minFlow");
        eraseBrush.maxPressureFlow = brushJson.at("maxFlow");
        eraseBrush.minPressureRadius = brushJson.at("minRadius");
        eraseBrush.maxPressureRadius = brushJson.at("maxRadius");
        eraseBrush.minPressureHardness = brushJson.at("minHardness");
        eraseBrush.maxPressureHardness = brushJson.at("maxHardness");
        eraseBrush.minPressureSpacing = brushJson.at("minSpacing");
        eraseBrush.maxPressureSpacing = brushJson.at("maxSpacing");
        eraseBrush.smoothing = brushJson.at("smoothing");
    }
}

void App::SaveBrushes() {
    ZoneScoped;
#ifdef NDEBUG
    const std::string path = SDL_GetPrefPath(nullptr, "midori");
#else
    const std::string path = SDL_GetPrefPath(nullptr, "midori-dev");
#endif

    {
        const std::string brushPath = std::format("{}brushes", path);
        SDL_CreateDirectory(brushPath.c_str());

        nlohmann::json brushJson = {
            {"color", {paintBrush.color.r, paintBrush.color.b, paintBrush.color.g, paintBrush.color.a}},
            {"minOpacity", paintBrush.minPressureOpacity},
            {"maxOpacity", paintBrush.maxPressureOpacity},
            {"minFlow", paintBrush.minPressureFlow},
            {"maxFlow", paintBrush.maxPressureFlow},
            {"minRadius", paintBrush.minPressureRadius},
            {"maxRadius", paintBrush.maxPressureRadius},
            {"minHardness", paintBrush.minPressureHardness},
            {"maxHardness", paintBrush.maxPressureHardness},
            {"minSpacing", paintBrush.minPressureSpacing},
            {"maxSpacing", paintBrush.maxPressureSpacing},
            {"smoothing", paintBrush.smoothing},
        };
        const std::string brushJsonDump = brushJson.dump(4);

        const std::string file = std::format("{}brushes/paintBrush.json", path);
        SDL_IOStream* fileIo = SDL_IOFromFile(file.c_str(), "w");
        SDL_assert(fileIo != nullptr);
        SDL_WriteIO(fileIo, brushJsonDump.data(), brushJsonDump.size());
        SDL_CloseIO(fileIo);
        paintBrush.modified = false;
    }
    {
        const std::string brushPath = std::format("{}brushes", path);
        SDL_CreateDirectory(brushPath.c_str());

        nlohmann::json brushJson = {
            {"color", {eraseBrush.color.r, eraseBrush.color.b, eraseBrush.color.g, eraseBrush.color.a}},
            {"minOpacity", eraseBrush.minPressureOpacity},
            {"maxOpacity", eraseBrush.maxPressureOpacity},
            {"minFlow", eraseBrush.minPressureFlow},
            {"maxFlow", eraseBrush.maxPressureFlow},
            {"minRadius", eraseBrush.minPressureRadius},
            {"maxRadius", eraseBrush.maxPressureRadius},
            {"minHardness", eraseBrush.minPressureHardness},
            {"maxHardness", eraseBrush.maxPressureHardness},
            {"minSpacing", eraseBrush.minPressureSpacing},
            {"maxSpacing", eraseBrush.maxPressureSpacing},
            {"smoothing", eraseBrush.smoothing},
        };
        const std::string brushJsonDump = brushJson.dump(4);

        const std::string file = std::format("{}brushes/eraseBrush.json", path);
        SDL_IOStream* fileIo = SDL_IOFromFile(file.c_str(), "w");
        SDL_assert(fileIo != nullptr);
        SDL_WriteIO(fileIo, brushJsonDump.data(), brushJsonDump.size());
        SDL_CloseIO(fileIo);
        eraseBrush.modified = false;
    }
}

void App::DebugTileCulling(glm::vec2 viewportSize, ImDrawList* drawList) {
    auto& canvasView = canvasViews.at(currentCanvasView.value());

    const auto tilePositions = canvasView.viewport.VisibleTiles(frameArena);
    for (const auto& tilePosition : tilePositions) {
        DrawTileDebug(tilePosition, drawList, IM_COL32(255, 0, 0, 128));
    }
}

// TODO use Viewport directly instead of relying on canvasView
void App::DrawTileDebug(glm::ivec2 pos, ImDrawList* drawList, ImU32 col, const std::string& label) const {
    const auto& canvasView = canvasViews.at(currentCanvasView.value());

    const glm::vec2 offsets[5] = {
        {canvasView.viewport.ViewMatrix()
         * glm::vec4((static_cast<glm::vec2>(pos) + glm::vec2(0.0f, 0.0f)) * glm::vec2(TILE_WIDTH, TILE_HEIGHT), 0.0f,
                     1.0f)},
        {canvasView.viewport.ViewMatrix()
         * glm::vec4((static_cast<glm::vec2>(pos) + glm::vec2(1.0f, 0.0f)) * glm::vec2(TILE_WIDTH, TILE_HEIGHT), 0.0f,
                     1.0f)},
        {canvasView.viewport.ViewMatrix()
         * glm::vec4((static_cast<glm::vec2>(pos) + glm::vec2(1.0f, 1.0f)) * glm::vec2(TILE_WIDTH, TILE_HEIGHT), 0.0f,
                     1.0f)},
        {canvasView.viewport.ViewMatrix()
         * glm::vec4((static_cast<glm::vec2>(pos) + glm::vec2(0.0f, 1.0f)) * glm::vec2(TILE_WIDTH, TILE_HEIGHT), 0.0f,
                     1.0f)},
        {canvasView.viewport.ViewMatrix()
         * glm::vec4((static_cast<glm::vec2>(pos) + glm::vec2(0.0f, 0.0f)) * glm::vec2(TILE_WIDTH, TILE_HEIGHT), 0.0f,
                     1.0f)},
    };

    const ImVec2 points[5]{
        ImVec2(std::floor(static_cast<float>(windowSize.x) / 2.0f) + offsets[0].x,
               std::floor(static_cast<float>(windowSize.y) / 2.0f) + offsets[0].y),
        ImVec2(std::ceil(static_cast<float>(windowSize.x) / 2.0f) + offsets[1].x,
               std::floor(static_cast<float>(windowSize.y) / 2.0f) + offsets[1].y),
        ImVec2(std::ceil(static_cast<float>(windowSize.x) / 2.0f) + offsets[2].x,
               std::ceil(static_cast<float>(windowSize.y) / 2.0f) + offsets[2].y),
        ImVec2(std::floor(static_cast<float>(windowSize.x) / 2.0f) + offsets[3].x,
               std::ceil(static_cast<float>(windowSize.y) / 2.0f) + offsets[3].y),
        ImVec2(std::floor(static_cast<float>(windowSize.x) / 2.0f) + offsets[4].x,
               std::floor(static_cast<float>(windowSize.y) / 2.0f) + offsets[4].y),
    };

    const ImVec2 center = ImVec2((points[0].x + points[1].x + points[2].x + points[3].x + points[4].x) / 5.0f,
                                 (points[0].y + points[1].y + points[2].y + points[3].y + points[4].y) / 5.0f);

    const auto text = std::format("[{},{}]", pos.x, pos.y);
    drawList->AddText(ImVec2(center.x, center.y), col, text.c_str());
    if (!label.empty()) {
        drawList->AddText(ImVec2(center.x, center.y + 20.0f), col, label.c_str());
    }
    drawList->AddPolyline(points, 5, col, 0, 1.0f);
}

} // namespace Midori
