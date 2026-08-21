#include "app.hpp"
#include "base.hpp"
#include "canvas.hpp"
#include "defines.hpp"
#include "errors.hpp"
#include "states.hpp"
#include "tiles.hpp"

#include <SDL3/SDL_log.h>
#include "../deps/json.hpp"
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlgpu3.h>
#include <algorithm>
#include <common/TracySystem.hpp>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <iterator>
#include <tracy/Tracy.hpp>
#include "../deps/qoi.h"

namespace Midori {

void WorkerThread(App* app);
void WriteThread(App* app);

App::App(int argc, char* argv[]) : renderer(this), stateManager(this) {
    ZoneScoped;
    args.resize(argc);
    pendingTileCommands.reserve(4_KiB);
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

    running = true;

    writeThread = std::thread(WriteThread, this);
    for (size_t i = 0; i < 2; i++) {
        workerThreads.emplace_back(WorkerThread, this);
    }

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

    OpenBrushes();

    const auto canvasFileId = canvasFiles.emplace(this);
    auto& canvasFile = canvasFiles.at(canvasFileId);
    canvasFile.Init(canvasFileId);

#ifdef NDEBUG
    const std::string path = SDL_GetPrefPath(nullptr, "midori");
#else
    const std::string path = SDL_GetPrefPath(nullptr, "midori-dev");
#endif
    auto filename = std::format("{}file.mido", path);
    if (std::filesystem::exists(filename)) {
        canvasFile.Open(filename);
    } else {
        canvasFile.New();
    }

    currentCanvasView = canvasViews.emplace(this, 0, canvasFileId);
    auto& canvasView = canvasViews.at(currentCanvasView.value());
    if (auto err = canvasView.Init(currentCanvasView.value()); err != Error::None) {
        return false;
    }
    canvasView.Resize(windowSize.x, windowSize.y);
    canvasView.SelectLayer(canvasFile.layersTree[0]);

    return true;
}

static const SDL_DialogFileFilter FILTERS[] = {
    {.name = "Midori files (.mido)", .pattern = "mido"},
    {.name = "All files", .pattern = "*"},
};

void App::DrawUI() {
    auto& canvasView = canvasViews.at(currentCanvasView.value());
    auto& canvasFile = canvasFiles.at(canvasView.canvasFileId);
    const size_t frameArenaUsage = frameArena.size();
    const size_t strokeArenaUsage = toolArena.size();

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
                for (const auto& [tile, state] : canvasFile.tileStates) {
                    // if (!layer.has_value() || (layer.has_value() && tile.layer == layer.value())) {
                    if (IsTileModified(state)) {
                        if (IsTileStaged(state)) {
                            ImU32 col = IM_COL32(0, 255, 0, 64);
                            const char* str{};
                            DrawTileDebug({tile.x, tile.y}, drawList, col, "Staged");
                        } else {
                            ImU32 col = IM_COL32(0, 255, 255, 64);
                            const char* str{};
                            DrawTileDebug({tile.x, tile.y}, drawList, col, "Modified");
                        }
                    } else {
                        ImU32 col = IM_COL32(0, 0, 0, 64);
                        DrawTileDebug({tile.x, tile.y}, drawList, col);
                    }
                    // }
                }
            }

            if (ImGui::Begin("Debug")) {
                ImGui::SeparatorText("Memory");
                ImGui::Text("Frame: %.03f/%.03fKB", static_cast<float>(frameArenaUsage) / 1_KB,
                            static_cast<float>(frameArena.capacity()) / 1_KB);
                ImGui::Text("Stroke: %.03f/%.03fKB", static_cast<float>(strokeArenaUsage) / 1_KB,
                            static_cast<float>(toolArena.capacity()) / 1_KB);
                ImGui::SeparatorText("IO");
                ImGui::SeparatorText("State");
                stateManager.DrawUI();
            }
            ImGui::End();

            if (!hideUi) {
                if (ImGui::Begin("Debug")) {
                    ImGui::Checkbox("view loaded tiles", &uiDebugCulling);
                    ImGui::LabelText("tile textures", "%zu", renderer.tileTextures.size());
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
}

void App::Update() {
    ZoneScoped;
    ++frame;
    frameArena.reset();

    DrawUI();

    for (auto [id, canvasView] : canvasViews) {
        canvasView.Update();
    }

    for (auto [id, canvasFile] : canvasFiles) {
        canvasFile.Update();
    }

    renderer.UpdateTileTextureUpload();
    renderer.UpdateTileTextureDownload();

    {
        ZoneScopedN("Polling tile transfers");
        std::vector<TileTransfer::ID> transferCompleted;
        for (const auto [id, tileTransfer] : tileTransfers) {
            SDL_assert(tileTransfer.error == Error::None);
            SDL_assert((tileTransfer.read == TileReadState::None) != (tileTransfer.write == TileWriteState::None)
                       && "A transfer must be either a read or a write");
            SDL_assert(canvasFiles.contains(tileTransfer.tile.canvas)
                       && "Transfer outlived the canvas file it belongs to");
            SDL_assert(tileTransfer.read != TileReadState::Uploaded
                       && "Read transfer stuck at Uploaded; nothing promotes it to Done");
            if (tileTransfer.read == TileReadState::Done) {
                SDL_assert(tileTransfer.write == TileWriteState::None);
                canvasFiles.at(tileTransfer.tile.canvas).tileStates.at(tileTransfer.tile) &= ~TileState::Transfering;
                canvasFiles.at(tileTransfer.tile.canvas).tileStates.at(tileTransfer.tile) |= TileState::Loaded;
                canvasFiles.at(tileTransfer.tile.canvas).tileTransfers.erase(tileTransfer.tile);
                transferCompleted.emplace_back(id);
            } else if (tileTransfer.write == TileWriteState::Done) {
                SDL_assert(tileTransfer.read == TileReadState::None);
                canvasFiles.at(tileTransfer.tile.canvas).tileStates.at(tileTransfer.tile) &= ~TileState::Transfering;
                canvasFiles.at(tileTransfer.tile.canvas).tileStates.at(tileTransfer.tile) |= TileState::Staged;
                canvasFiles.at(tileTransfer.tile.canvas).tileTransfers.erase(tileTransfer.tile);
                transferCompleted.emplace_back(id);
            } else if (tileTransfer.write == TileWriteState::Downloaded) {
                // Claim the transfer before queueing so the next frame cannot dispatch a second encode for it
                tileTransfer.write = TileWriteState::Encoding;
                const WorkerCommand workerCommand{
                    .type = WorkerCommand::Type::Write,
                    .canvasId = tileTransfer.tile.canvas,
                    .staged = true,
                    .tile = tileTransfer.tile,
                    .id = id,
                };
                {
                    std::unique_lock lock(workerMutex);
                    workerQueue.push(workerCommand);
                }
                workerCv.notify_one();
            }
        }
        for (const auto& id : transferCompleted) {
            tileTransfers.erase(id);
        }
    }

    {
        ZoneScopedN("Ordering tile commands");
        {
            std::unique_lock lock(tileCommandsMutex);
            pendingTileCommands.insert(pendingTileCommands.end(), std::make_move_iterator(tileCommandsQueue.begin()),
                                       std::make_move_iterator(tileCommandsQueue.end()));
            tileCommandsQueue.clear();
        }

        std::ranges::sort(pendingTileCommands, [](const TileCommand& a, const TileCommand& b) {
            return std::tie(a.type, a.srcTile.canvas, a.srcTile.layer, a.srcTile.y, a.srcTile.x, a.srcTile.mip, a.seq)
                 < std::tie(b.type, b.srcTile.canvas, b.srcTile.layer, b.srcTile.y, b.srcTile.x, b.srcTile.mip, b.seq);
        });
        // A tile edit that gets deduplicated here was still counted when it was recorded, so give its count back or
        // pendingTileEdits can never return to 0.
        for (size_t i = 1; i < pendingTileCommands.size(); i++) {
            const auto& a = pendingTileCommands[i - 1];
            const auto& b = pendingTileCommands[i];
            if ((b.type == TileCommand::Type::Paint || b.type == TileCommand::Type::Erase)
                && std::tie(a.type, a.srcTile, a.dstTile) == std::tie(b.type, b.srcTile, b.dstTile)) {
                SDL_assert(pendingTileEdits > 0 && "Dropping a tile edit that was never counted");
                pendingTileEdits--;
            }
        }

        auto last = std::ranges::unique(pendingTileCommands, [](const TileCommand& a, const TileCommand& b) {
            return std::tie(a.type, a.srcTile, a.dstTile) == std::tie(b.type, b.srcTile, b.dstTile);
        });
        pendingTileCommands.erase(last.begin(), last.end());
    }

    {
        ZoneScopedN("Executing tile commands");

        size_t rescheduled{};
        size_t commandTypeCount{};
        TileCommand::Type prev{TileCommand::Type::None};
        for (size_t i = 0; i < pendingTileCommands.size(); i++) {
            bool first{};
            bool last{};
            {
                // Is this tile command the first and/or the last of it's type
                commandTypeCount++; // Useful for batching etc...
                if ((prev != pendingTileCommands[i].type) || i == 0) {
                    first = true;
                    commandTypeCount = 0;
                }
                if (i + 1 < pendingTileCommands.size()) {
                    if (pendingTileCommands[i].type != pendingTileCommands[i + 1].type) {
                        last = true;
                    }
                } else if (i + 1 == pendingTileCommands.size()) {
                    last = true;
                }
            }
            switch (pendingTileCommands[i].type) {
                case TileCommand::Type::New: {
                    if (auto res = NewTileCommand(pendingTileCommands[i].srcTile, first, last); res != Error::None) {
                        // pendingTileCommands[rescheduled++] = pendingTileCommands[i];
                    }
                } break;
                case TileCommand::Type::Load: {
                    if (auto res = LoadTileCommand(pendingTileCommands[i].srcTile, first, last); res != Error::None) {
                        pendingTileCommands[rescheduled++] = pendingTileCommands[i];
                    }
                } break;
                case TileCommand::Type::Duplicate: {
                    if (auto res = DuplicateTileCommand(pendingTileCommands[i].srcTile, pendingTileCommands[i].dstTile,
                                                        first, last);
                        res != Error::None) {
                        pendingTileCommands[rescheduled++] = pendingTileCommands[i];
                    }
                } break;
                case TileCommand::Type::Merge: {
                    const Renderer::MergeInfo info{
                        .blendMode = pendingTileCommands[i].blendMode,
                        .opacity = pendingTileCommands[i].opacity,
                    };
                    if (auto res = MergeTileCommand(pendingTileCommands[i].srcTile, pendingTileCommands[i].dstTile,
                                                    info, first, last);
                        res != Error::None) {
                        pendingTileCommands[rescheduled++] = pendingTileCommands[i];
                    }
                } break;
                case TileCommand::Type::Paint: {
                    if (auto res = PaintTileCommand(pendingTileCommands[i].srcTile, first, last); res != Error::None) {
                        pendingTileCommands[rescheduled++] = pendingTileCommands[i];
                    }
                } break;
                case TileCommand::Type::Erase: {
                    if (auto res = EraseTileCommand(pendingTileCommands[i].srcTile, first, last); res != Error::None) {
                        pendingTileCommands[rescheduled++] = pendingTileCommands[i];
                    }
                } break;
                case TileCommand::Type::Render: {
                    if (auto res = RenderTileCommand(pendingTileCommands[i].srcTile, first, last); res != Error::None) {
                        pendingTileCommands[rescheduled++] = pendingTileCommands[i];
                    }
                } break;
                case TileCommand::Type::Stage: {
                    if (auto res = StageTileCommand(pendingTileCommands[i].srcTile, first, last); res != Error::None) {
                        pendingTileCommands[rescheduled++] = pendingTileCommands[i];
                    }

                } break;
                case TileCommand::Type::Delete: {
                    if (auto res = DeleteTileCommand(pendingTileCommands[i].srcTile, first, last); res != Error::None) {
                        pendingTileCommands[rescheduled++] = pendingTileCommands[i];
                    }
                } break;

                case TileCommand::Type::Destroy: {
                    if (auto res = DestroyTileCommand(pendingTileCommands[i].srcTile, first, last);
                        res != Error::None) {
                        pendingTileCommands[rescheduled++] = pendingTileCommands[i];
                    }
                } break;
                default: {
                    SDL_assert(false && "TileCommand Type not suported");
                } break;
            }

            {
                // Issue Update to the different subsytem if some conditions are met
                if (pendingTileCommands[i].type == TileCommand::Type::Load) {
                    if (last) {
                        for (auto& buf : renderer.tileUploadStagingBuffers) {
                            std::unique_lock lock(buf.mutex);
                            if (buf.commited.any() && buf.used.count() == buf.commited.count()) {
                                buf.state = Renderer::StagingBufferState::Completed;
                            }
                        }
                    }
                    if (last || ((commandTypeCount % TILE_UPLOAD_BUFFER_CAPACITY == 0) && !first)) {
                        renderer.UpdateTileTextureUpload();
                    }
                } else if (pendingTileCommands[i].type == TileCommand::Type::Stage) {
                    if (last) {
                        for (auto& buf : renderer.tileDownloadStagingBuffers) {
                            std::unique_lock lock(buf.mutex);
                            if (buf.commited.any() && buf.used.count() == buf.commited.count()) {
                                buf.state = Renderer::StagingBufferState::Completed;
                            }
                        }
                    }
                    if (last || ((commandTypeCount % TILE_DOWNLOAD_BUFFER_CAPACITY == 0) && !first)) {
                        renderer.UpdateTileTextureDownload();
                    }
                } else if (pendingTileCommands[i].type == TileCommand::Type::New && last) {
                    renderer.UpdateUninitializedTextures();
                }
            }

            prev = pendingTileCommands[i].type;
        }
        pendingTileCommands.resize(rescheduled);
    }

    for (const auto& [id, canvasView] : canvasViews) {
        renderer.RenderCanvasView(canvasView);
    }

    // Only do this every V-Blank
    renderer.Render();
}

Error App::DuplicateTileCommand(Tile srcTile, Tile dstTile, bool first, bool last) {
    ZoneScoped;
    SDL_assert(canvasFiles.contains(srcTile.canvas) && "Duplicate on a canvas file that does not exist");
    SDL_assert(srcTile.canvas == dstTile.canvas && "Duplicate across canvas files");
    SDL_assert(!(srcTile == dstTile) && "Duplicate onto itself");

    // DuplicateTileTexture() records into a copy pass, so the whole run shares one command buffer.
    static bool batchOpen{};

    if (first) {
        SDL_assert(!batchOpen && "A duplicate batch was left open by the previous run");
        if (!renderer.BeginCopyPass()) {
            return Error::Unknown;
        }
        batchOpen = true;
    }
    SDL_assert(batchOpen && "Duplicate command outside of an open batch");

    Error result = Error::None;
    auto& canvasFile = canvasFiles.at(srcTile.canvas);

    if (!canvasFile.tileStates.contains(srcTile)) {
        result = Error::TileNotFound;
    } else {
        const auto srcTileState = canvasFile.tileStates.at(srcTile);
        if (!IsTileLoaded(srcTileState) || IsTileTransfering(srcTileState)) {
            result = Error::TileNotFound;
        } else {
            SDL_assert(!renderer.tileTextures.contains(dstTile) && "Duplicate destination already has a gpu texture");
            SDL_assert(canvasFile.layers.contains(dstTile.layer) && "Duplicate destination layer does not exist");

            auto* texture = renderer.DuplicateTileTexture(srcTile);
            if (!texture) {
                SDL_assert(false && "Failed to duplicate the tile gpu texture");
                result = Error::Unknown;
            } else if (!renderer.tileTextures.insert(dstTile, texture)) {
                SDL_assert(false && "Failed to insert the duplicated tile texture");
                result = Error::OutOfCapacity;
            } else {
                canvasFile.layers.at(dstTile.layer).tiles.insert(dstTile);
                canvasFile.tileStates.insert(dstTile, TileState::Loaded | TileState::Modified);
            }
        }
    }

    if (last) {
        renderer.EndCopyPass(false);
        batchOpen = false;
    }

    return result;
}

Error App::NewTileCommand(Tile tile, bool first, bool last) {
    ZoneScoped;

    SDL_assert(canvasFiles.contains(tile.canvas) && "New on a canvas file that does not exist");
    auto& canvasFile = canvasFiles.at(tile.canvas);
    SDL_assert(!canvasFile.tileStates.contains(tile));
    SDL_assert(canvasFile.layers.contains(tile.layer) && "New on a layer that does not exist");
    SDL_assert(!canvasFile.tileTransfers.contains(tile) && "New on a tile that has a transfer in flight");

    auto& layerInfo = canvasFile.layers.at(tile.layer);
    if (layerInfo.tiles.full()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Layer %u is at its tile limit", tile.layer);
        return Error::OutOfCapacity;
    }

    if (auto err = renderer.CreateTileTexture(tile); err != Error::None) {
        SDL_assert(false && "Failed to create the tile gpu texture");
        return Error::Unknown;
    }

    layerInfo.tiles.insert(tile);
    canvasFile.tileStates.insert(tile, TileState::Modified | TileState::Loaded);

    return Error::None;
}

Error App::LoadTileCommand(Tile tile, bool first, bool last) {
    ZoneScoped;

    SDL_assert(canvasFiles.contains(tile.canvas) && "Load on a canvas file that does not exist");
    SDL_assert(canvasFiles.at(tile.canvas).tileStates.contains(tile) && "Load on a tile with no state entry");
    SDL_assert(!canvasFiles.at(tile.canvas).tileTransfers.contains(tile)
               && "Load on a tile that already has a transfer in flight");

    if (tileTransfers.full()) {
        return Error::OutOfCapacity;
    }
    auto& tileState = canvasFiles.at(tile.canvas).tileStates.at(tile);
    SDL_assert(!IsTileLoaded(tileState) && "Load on a tile that is already loaded");
    SDL_assert(!IsTileTransfering(tileState) && "Load on a tile that is already transfering");
    SDL_assert(!IsTileDeleted(tileState) && "Load on a tile that is staged as deleted");
    tileState |= TileState::Transfering; // NOTE Is this correct

    const auto id = tileTransfers.emplace();
    auto& tileTransfer = tileTransfers.at(id);
    tileTransfer.tile = tile;
    tileTransfer.read = TileReadState::Pending;
    canvasFiles.at(tile.canvas).tileTransfers.insert(tile, id);

    const WorkerCommand workerCommand{
        .type = WorkerCommand::Type::Read,
        .canvasId = tile.canvas,
        .staged = IsTileStaged(tileState),
        .tile = tile,
        .id = id,
    };
    {
        std::unique_lock lock(workerMutex);
        workerQueue.push(workerCommand);
    }
    workerCv.notify_one();

    return Error::None;
}

Error App::MergeTileCommand(Tile srcTile, Tile dstTile, Renderer::MergeInfo info, bool first, bool last) {
    ZoneScoped;
    SDL_assert(canvasFiles.contains(srcTile.canvas) && "Merge on a canvas file that does not exist");
    SDL_assert(srcTile.canvas == dstTile.canvas && "Merge across canvas files");
    SDL_assert(!(srcTile == dstTile) && "Merge onto itself");

    // Renderer::MergeTiles() takes one canvas file and one MergeInfo per call, so the batch also breaks when the
    // canvas or the blending changes, not only on the run boundaries.
    static std::vector<Tile> srcBatch;
    static std::vector<Tile> dstBatch;
    static CanvasFileID batchCanvas{};
    static Renderer::MergeInfo batchInfo{};

    if (first) {
        srcBatch.clear();
        dstBatch.clear();
    }

    if (!srcBatch.empty()
        && (batchCanvas != srcTile.canvas || batchInfo.blendMode != info.blendMode
            || batchInfo.opacity != info.opacity)) {
        renderer.MergeTiles(canvasFiles.at(batchCanvas), srcBatch, dstBatch, batchInfo);
        srcBatch.clear();
        dstBatch.clear();
    }

    Error result = Error::None;
    auto& canvasFile = canvasFiles.at(srcTile.canvas);
    if (!canvasFile.tileStates.contains(srcTile) || !canvasFile.tileStates.contains(dstTile)) {
        result = Error::TileNotFound;
    } else {
        const auto srcTileState = canvasFile.tileStates.at(srcTile);
        const auto dstTileState = canvasFile.tileStates.at(dstTile);
        if (!IsTileLoaded(srcTileState) || IsTileTransfering(srcTileState) || !IsTileLoaded(dstTileState)
            || IsTileTransfering(dstTileState)) {
            result = Error::TileNotFound;
        } else {
            srcBatch.push_back(srcTile);
            dstBatch.push_back(dstTile);
            batchCanvas = srcTile.canvas;
            batchInfo = info;
            canvasFile.tileStates.at(dstTile) |= TileState::Modified;
        }
    }

    if (last && !srcBatch.empty()) {
        renderer.MergeTiles(canvasFiles.at(batchCanvas), srcBatch, dstBatch, batchInfo);
        srcBatch.clear();
        dstBatch.clear();
    }

    return result;
}

Error App::PaintTileCommand(Tile tile, bool first, bool last) {
    ZoneScoped;
    SDL_assert(canvasFiles.contains(tile.canvas) && "Paint on a canvas file that does not exist");
    SDL_assert(pendingTileEdits > 0 && "Paint command with no recorded pending edit");

    static std::vector<Tile> batch;
    static CanvasFileID batchCanvas{};

    if (first) {
        batch.clear();
    }

    if (!batch.empty() && batchCanvas != tile.canvas) {
        renderer.PaintTiles(canvasFiles.at(batchCanvas), batch, strokePoints);
        strokePoints.clear();
        strokeTiles.clear();
        pendingTileEdits -= static_cast<uint32_t>(batch.size());
        batch.clear();
    }

    Error result = Error::None;
    auto& canvasFile = canvasFiles.at(tile.canvas);
    if (!canvasFile.tileStates.contains(tile)) {
        result = Error::TileNotFound;
    } else {
        const auto tileState = canvasFile.tileStates.at(tile);
        if (!IsTileLoaded(tileState) || IsTileTransfering(tileState)) {
            result = Error::TileNotFound;
        } else {
            batch.push_back(tile);
            batchCanvas = tile.canvas;
            canvasFile.tileStates.at(tile) |= TileState::Modified;
        }
    }

    if (last && !batch.empty()) {
        renderer.PaintTiles(canvasFiles.at(batchCanvas), batch, strokePoints);
        strokePoints.clear();
        strokeTiles.clear();
        pendingTileEdits -= static_cast<uint32_t>(batch.size());
        batch.clear();
    }

    return result;
}

Error App::EraseTileCommand(Tile tile, bool first, bool last) {
    ZoneScoped;
    SDL_assert(canvasFiles.contains(tile.canvas) && "Erase on a canvas file that does not exist");
    SDL_assert(pendingTileEdits > 0 && "Erase command with no recorded pending edit");

    static std::vector<Tile> batch;
    static CanvasFileID batchCanvas{};

    if (first) {
        batch.clear();
    }

    if (!batch.empty() && batchCanvas != tile.canvas) {
        renderer.EraseTiles(canvasFiles.at(batchCanvas), batch, strokePoints);
        strokePoints.clear();
        strokeTiles.clear();
        pendingTileEdits -= static_cast<uint32_t>(batch.size());
        batch.clear();
    }

    Error result = Error::None;
    auto& canvasFile = canvasFiles.at(tile.canvas);
    if (!canvasFile.tileStates.contains(tile)) {
        result = Error::TileNotFound;
    } else {
        const auto tileState = canvasFile.tileStates.at(tile);
        if (!IsTileLoaded(tileState) || IsTileTransfering(tileState)) {
            result = Error::TileNotFound;
        } else {
            batch.push_back(tile);
            batchCanvas = tile.canvas;
            canvasFile.tileStates.at(tile) |= TileState::Modified;
        }
    }

    if (last && !batch.empty()) {
        renderer.EraseTiles(canvasFiles.at(batchCanvas), batch, strokePoints);
        strokePoints.clear();
        strokeTiles.clear();
        pendingTileEdits -= static_cast<uint32_t>(batch.size());
        batch.clear();
    }

    return result;
}

Error App::RenderTileCommand(Tile tile, bool first, bool last) {
    ZoneScoped;
    SDL_assert(canvasFiles.contains(tile.canvas) && "Render on a canvas file that does not exist");

    // static std::vector<CanvasFileID> touchedCanvases;

    if (first) {
        // touchedCanvases.clear();
        renderer.tileToDraw.clear();
    }

    Error result = Error::None;
    auto& canvasFile = canvasFiles.at(tile.canvas);
    if (!canvasFile.tileStates.contains(tile) || !IsTileLoaded(canvasFile.tileStates.at(tile))) {
        result = Error::TileNotFound;
    } else {
        renderer.tileToDraw.insert(tile);
        // if (std::ranges::find(touchedCanvases, tile.canvas) == touchedCanvases.end()) {
        //     touchedCanvases.push_back(tile.canvas);
        // }
    }

    // if (last) {
    //     for (auto [id, canvasView] : canvasViews) {
    //         const auto viewCanvas = static_cast<CanvasFileID>(canvasView.canvasFileId);
    //         if (std::ranges::find(touchedCanvases, viewCanvas) != touchedCanvases.end()) {
    //             renderer.RenderCanvasView(canvasView);
    //         }
    //     }
    // touchedCanvases.clear();
    // }

    return result;
}

Error App::StageTileCommand(Tile tile, bool first, bool last) {
    ZoneScoped;
    SDL_assert(canvasFiles.contains(tile.canvas) && "Stage on a canvas file that does not exist");
    SDL_assert(canvasFiles.at(tile.canvas).tileStates.contains(tile) && "Stage on a tile with no state entry");
    SDL_assert(!canvasFiles.at(tile.canvas).tileTransfers.contains(tile)
               && "Stage on a tile that already has a transfer in flight");

    if (tileTransfers.full()) {
        return Error::OutOfCapacity;
    }

    auto& tileState = canvasFiles.at(tile.canvas).tileStates.at(tile);
    SDL_assert(IsTileModified(tileState) && "Stage on a tile that was never modified");
    if (!IsTileLoaded(tileState)) {
        SDL_assert(IsTileTransfering(tileState));
        return Error::Unknown;
    }
    SDL_assert(!IsTileTransfering(tileState) && "Stage on a loaded tile that is already transfering");

    const auto id = tileTransfers.emplace();
    auto& tileTransfer = tileTransfers.at(id);
    tileTransfer.tile = tile;
    tileTransfer.write = TileWriteState::Pending;

    auto slot = renderer.QueueTileDownloadSlot(id);
    if (slot.has_value()) {
        tileTransfers.at(id).slot = slot.value();
        canvasFiles.at(tile.canvas).tileTransfers.insert(tile, id);
        tileState |= TileState::Transfering; // NOTE Is this correct
    } else {
        tileTransfers.erase(id);
        return Error::Unknown;
    }
    return Error::None;
}

Error App::DeleteTileCommand(Tile tile, bool first, bool last) {
    ZoneScoped;
    SDL_assert(canvasFiles.contains(tile.canvas) && "Delete on a canvas file that does not exist");
    auto& canvasFile = canvasFiles.at(tile.canvas);
    SDL_assert(canvasFile.tileStates.contains(tile) && "Delete on a tile with no state entry");
    SDL_assert(canvasFile.stageEnv && "Delete on a canvas file with no stage env");

    auto& tileState = canvasFile.tileStates.at(tile);
    if (IsTileTransfering(tileState)) {
        return Error::Unknown;
    }

    tileState |= TileState::Deleted | TileState::Modified;

    WriteCommand writeCommand{};
    const auto key = PackTileKey(tile);
    std::memcpy(writeCommand.key, &key, sizeof(key));
    writeCommand.keySize = sizeof(key);
    writeCommand.type = WriteCommand::Type::StageDel;
    writeCommand.canvas = tile.canvas;
    writeCommand.dbi = canvasFile.stageTiles;
    writeCommand.id = TileTransfer::NONE;

    {
        std::unique_lock lock(writeMutex);
        writeQueue.push_back(writeCommand);
    }
    writeCv.notify_one();

    return Error::None;
}

Error App::DestroyTileCommand(Tile tile, bool first, bool last) {
    ZoneScoped;
    SDL_assert(canvasFiles.contains(tile.canvas) && "Destroy on a canvas file that does not exist");
    auto& canvasFile = canvasFiles.at(tile.canvas);
    SDL_assert(canvasFile.tileStates.contains(tile) && "Destroy on a tile with no state entry");

    auto& tileState = canvasFile.tileStates.at(tile);
    if (IsTileTransfering(tileState)) {
        return Error::Unknown;
    }
    // SDL_assert((IsTileModified(tileState) || IsTileStaged(tileState) || IsTileDeleted(tileState))
    //            && "Destroy would drop unstaged modifications");

    if (renderer.tileTextures.contains(tile)) {
        renderer.DestroyTileTexture(tile);
    }
    tileState &= ~TileState::Loaded;

    return Error::None;
}

bool App::Resize(const int width, const int height) {
    ZoneScoped;
    windowSize.x = width;
    windowSize.y = height;

    if (windowSize.x > 0 || windowSize.y > 0) {
        for (auto [id, canvasView] : canvasViews) {
            canvasView.Resize(windowSize.x, windowSize.y);
        }
    }

    return true;
}

void App::ShouldQuit() {
    shouldQuit = true;

    for (auto [id, canvasFile] : canvasFiles) {
        canvasFile.canvasHistory.Clear();
    }
}

bool App::CanQuit() const {
    ZoneScoped;

    if (!tileTransfers.empty()) {
        return false;
    }

    if (!renderer.CanQuit()) {
        return false;
    }

    return true;
}

void App::Quit() {
    ZoneScoped;

    running = false;
    writeCv.notify_all();
    workerCv.notify_all();

    for (auto& workerThread : workerThreads) {
        workerThread.join();
    }

    writeThread.join();

    for (auto [id, canvasView] : canvasViews) {
        canvasView.Quit();
    }

    for (auto [id, canvasFile] : canvasFiles) {
        canvasFile.Close();
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
        SDL_assert(fileIo != nullptr && "Failed to open the brush file for writing");
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
        SDL_assert(fileIo != nullptr && "Failed to open the brush file for writing");
        SDL_WriteIO(fileIo, brushJsonDump.data(), brushJsonDump.size());
        SDL_CloseIO(fileIo);
        eraseBrush.modified = false;
    }
}

void App::DebugTileCulling(glm::vec2 viewportSize, ImDrawList* drawList) {
    ZoneScoped;
    auto& canvasView = canvasViews.at(currentCanvasView.value());

    const auto tilePositions = canvasView.viewport.VisibleTiles(frameArena);
    for (const auto& tilePosition : tilePositions) {
        DrawTileDebug(tilePosition, drawList, IM_COL32(255, 0, 0, 128));
    }
}

// TODO use Viewport directly instead of relying on canvasView
void App::DrawTileDebug(glm::ivec2 pos, ImDrawList* drawList, ImU32 col, const std::string& label) const {
    ZoneScoped;
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

void WorkerThread(App* app) {
    tracy::SetThreadName("Worker");
    thread_local MDB_txn* fileTxn[CANVASFILE_MAX]{};
    thread_local MDB_txn* stageTxn[CANVASFILE_MAX]{};

    while (true) {
        WorkerCommand command;
        {
            std::unique_lock lock(app->workerMutex);
            app->workerCv.wait(lock, [app] { return !app->workerQueue.empty() || !app->running; });
            if (!app->workerQueue.empty()) {
                command = app->workerQueue.front();
                app->workerQueue.pop();
            } else if (!app->running) {
                ZoneScopedN("Quit thread");
                for (auto& txn : fileTxn) {
                    mdb_txn_abort(txn);
                }
                for (auto& txn : fileTxn) {
                    mdb_txn_abort(txn);
                }
                return;
            } else {
                continue;
            }
        }

        switch (command.type) {
            case WorkerCommand::Type::Read: {
                ZoneScopedN("Load");

                SDL_assert(app->canvasFiles.contains(command.tile.canvas));
                SDL_assert(app->tileTransfers.contains(command.id));
                SDL_assert(command.canvasId == command.tile.canvas
                           && "Worker command canvas id does not match the tile it reads");

                const auto& canvas = app->canvasFiles.at(command.tile.canvas);
                auto& transfer = app->tileTransfers.at(command.id);

                MDB_val data;
                void* buf;

                {
                    ZoneScopedN("Read");
                    int rc;
                    MDB_val key{
                        .mv_size = sizeof(uint64_t),
                        .mv_data = PackTileKey(command.tile),
                    };
                    if (command.staged) {
                        if (!stageTxn[command.canvasId]) {
                            rc = mdb_txn_begin(canvas.stageEnv, nullptr, MDB_RDONLY, &stageTxn[command.canvasId]);
                        } else {
                            rc = mdb_txn_renew(stageTxn[command.canvasId]);
                        }
                        rc = mdb_get(stageTxn[command.canvasId], canvas.stageTiles, &key, &data);
                    } else {
                        if (!fileTxn[command.canvasId]) {
                            rc = mdb_txn_begin(canvas.fileEnv, nullptr, MDB_RDONLY, &fileTxn[command.canvasId]);
                        } else {
                            rc = mdb_txn_renew(fileTxn[command.canvasId]);
                        }
                        rc = mdb_get(fileTxn[command.canvasId], canvas.fileTiles, &key, &data);
                    }
                    SDL_assert(rc == MDB_SUCCESS && "Failed to read the tile from the canvas file");
                    SDL_assert(data.mv_data && data.mv_size > 0 && "Tile entry is empty");
                    // Update thejob.transactionID
                    transfer.read = TileReadState::Read;
                }

                {
                    ZoneScopedN("Decoded");
                    qoi_desc desc;
                    buf = qoi_decode(data.mv_data, static_cast<int>(data.mv_size), &desc, 4);
                    // if it fails set an error state for job.transactionID
                    SDL_assert(buf && "Failed to decode the tile");
                    SDL_assert(desc.width == TILE_WIDTH && desc.height == TILE_HEIGHT
                               && "Decoded tile has the wrong size");
                    SDL_assert(desc.channels == 4 && "Decoded tile has the wrong channel count");
                    app->tileTransfers.at(command.id).read = TileReadState::Decoded;

                    if (command.staged) {
                        mdb_txn_reset(stageTxn[command.canvasId]);
                    } else {
                        mdb_txn_reset(fileTxn[command.canvasId]);
                    }
                }

                {
                    ZoneScopedN("Submit");
                    size_t i{};
                    auto slot = app->renderer.ReserveTileUploadSlot(command.id);
                    while (!slot.has_value()) {
                        slot = app->renderer.ReserveTileUploadSlot(command.id);
                    }
                    transfer.slot = slot.value();

                    auto* slotData = app->renderer.GetTileUploadSlotData(slot.value());
                    SDL_assert(slotData && "Upload slot data is not writable");
                    std::memcpy(slotData, buf, TILE_SIZE);
                    app->renderer.CommitTileUploadSlot(slot.value());
                }
                Free(buf);
            } break;
            case WorkerCommand::Type::Write: {
                ZoneScopedN("Unload");
                SDL_assert(app->canvasFiles.contains(command.tile.canvas));
                SDL_assert(app->tileTransfers.contains(command.id));

                const auto& canvas = app->canvasFiles.at(command.tile.canvas);
                auto& transfer = app->tileTransfers.at(command.id);
                SDL_assert(app->tileTransfers.at(command.id).write == TileWriteState::Encoding
                           && "Encode worker command for a transfer that was not claimed for encoding");

                qoi_desc desc{
                    .width = TILE_WIDTH,
                    .height = TILE_HEIGHT,
                    .channels = 4,
                    .colorspace = QOI_LINEAR,
                };

                void* data{};
                {
                    ZoneScopedN("Getting");
                    while (app->renderer.tileDownloadStagingBuffers[transfer.slot.buf].state
                           != Renderer::StagingBufferState::Done) {}

                    data = app->renderer.GetTileDownloadSlotData(transfer.slot);
                    SDL_assert(data && "Download slot data is not readable");
                }

                void* buf{};
                int len{};
                {
                    ZoneScopedN("Encode");
                    buf = qoi_encode(data, &desc, &len);
                    SDL_assert(buf && "Failed to encode the tile");
                    SDL_assert(len > 0 && "Encoded tile is empty");
                    app->renderer.FreeTileDownloadSlot(transfer.slot);
                    app->tileTransfers.at(command.id).write = TileWriteState::Encoded;
                }

                {
                    ZoneScopedN("Send to write");
                    WriteCommand writeCommand{};
                    static_assert(std::size(writeCommand.key) >= sizeof(uint64_t));
                    const auto tileKey = PackTileKey(command.tile);
                    std::memcpy(writeCommand.key, &tileKey, sizeof(tileKey));
                    writeCommand.keySize = sizeof(tileKey);
                    writeCommand.canvas = command.tile.canvas;
                    writeCommand.type = WriteCommand::Type::StagePut;
                    writeCommand.dbi = canvas.stageTiles;
                    writeCommand.data = buf;
                    writeCommand.dataSize = len;
                    writeCommand.id = command.id;
                    {
                        // TODO Switch to MPSC for better perfs ¯\_(ツ)_/¯
                        std::unique_lock lock(app->writeMutex);
                        app->writeQueue.push_back(writeCommand);
                    }
                    app->writeCv.notify_one();
                }
            } break;
            default:
                SDL_assert(false && "WorkerCommand Type not supported");
                break;
        }
    }
}

void WriteThread(App* app) {
    tracy::SetThreadName("Writer");
    thread_local std::vector<WriteCommand> batchedWriteCommands;
    batchedWriteCommands.reserve(512);

    while (true) {
        if (!batchedWriteCommands.empty()) {
            batchedWriteCommands.clear();
        }
        {
            std::unique_lock lock(app->writeMutex);
            app->writeCv.wait(lock, [app] { return !app->writeQueue.empty() || !app->running; });
            if (!app->writeQueue.empty()) {
                ZoneScopedN("Empty pending writes");
                batchedWriteCommands.assign(std::make_move_iterator(app->writeQueue.begin()),
                                            std::make_move_iterator(app->writeQueue.end()));
                app->writeQueue.clear();
            } else if (!app->running) {
                ZoneScopedN("Quit thread");
                SDL_assert(app->writeQueue.empty() && "Shutting down the writer with pending writes");
                return;
            } else {
                SDL_assert(app->writeQueue.empty() && "Woke with pending writes but skipped the drain branch");
                continue;
            }
        }

        std::ranges::sort(batchedWriteCommands, [](const WriteCommand& a, const WriteCommand& b) {
            return std::tie(a.canvas, a.type, a.dbi) > std::tie(b.canvas, b.type, b.dbi);
        });

        {
            ZoneScopedN("Writing");
            MDB_txn* txn{};
            CanvasFile* prevCanvas{};
            bool prevStaged{};
            int rc{};

            for (auto& writeCommand : batchedWriteCommands) {
                ZoneScopedN("Command");
                MDB_val key{
                    .mv_size = writeCommand.keySize,
                    .mv_data = writeCommand.key,
                };
                MDB_val data{
                    .mv_size = writeCommand.dataSize,
                    .mv_data = writeCommand.data,
                };

                bool staged;
                switch (writeCommand.type) {
                    case WriteCommand::Type::StagePut:
                    case WriteCommand::Type::StageDel:
                        staged = true;
                        break;
                    case WriteCommand::Type::Commit:
                    case WriteCommand::Type::Put:
                    case WriteCommand::Type::Del:
                        staged = false;
                        break;
                    default:
                        SDL_assert(false && "Unknown Write operation");
                        break;
                }

                auto& canvas = app->canvasFiles.at(writeCommand.canvas);
                if (txn && (prevCanvas != &canvas || prevStaged != staged)) {
                    ZoneScopedN("Commit txn");
                    rc = mdb_txn_commit(txn);
                    txn = nullptr;
                }

                switch (writeCommand.type) {
                    case WriteCommand::Type::StagePut: {
                        ZoneScopedN("StagePut");
                        if (!txn) {
                            rc = mdb_txn_begin(canvas.stageEnv, nullptr, 0, &txn);
                        }
                        SDL_assert(writeCommand.dbi == canvas.stageTiles);
                        rc = mdb_put(txn, writeCommand.dbi, &key, &data, 0);
                        if (writeCommand.id != TileTransfer::NONE) {
                            app->tileTransfers.at(writeCommand.id).write = TileWriteState::Done;
                        }
                        Free(writeCommand.data);
                    } break;
                    case WriteCommand::Type::StageDel: {
                        ZoneScopedN("StageDel");
                        if (!txn) {
                            rc = mdb_txn_begin(canvas.stageEnv, nullptr, 0, &txn);
                        }
                        SDL_assert(writeCommand.dbi == canvas.stageTiles);
                        rc = mdb_del(txn, writeCommand.dbi, &key, nullptr);
                        if (writeCommand.id != TileTransfer::NONE) {
                            app->tileTransfers.at(writeCommand.id).write = TileWriteState::Done;
                        }
                    } break;
                    case WriteCommand::Type::Commit: {
                        ZoneScopedN("Commit");
                        // Big operations
                        // This move any modified (not deleted) tiles from stageEnv.stageTiles to env.tiles
                        // For deleted layers we use the flag set inside of TileState to know if it was deleted
                        // We can use the openned stateWriteTxn as a read-only
                        // Once the transfer is done we erase any data inside of stageEnv.stageTiles and reflect it
                        // on the tileState
                        // We also delete any entry inside of tileState that are not valid
                        // This can be done from another thread
                    } break;
                    case WriteCommand::Type::Put: {
                        ZoneScopedN("Put");
                        const auto& canvas = app->canvasFiles.at(writeCommand.canvas);
                        if (!txn) {
                            rc = mdb_txn_begin(canvas.fileEnv, nullptr, 0, &txn);
                        }
                        rc = mdb_put(txn, writeCommand.dbi, &key, &data, 0);
                        if (writeCommand.id != TileTransfer::NONE) {
                            app->tileTransfers.at(writeCommand.id).write = TileWriteState::Done;
                        }
                        Free(writeCommand.data);
                    } break;
                    case WriteCommand::Type::Del: {
                        ZoneScopedN("Del");
                        const auto& canvas = app->canvasFiles.at(writeCommand.canvas);
                        if (!txn) {
                            rc = mdb_txn_begin(canvas.fileEnv, nullptr, 0, &txn);
                        }
                        rc = mdb_del(txn, writeCommand.dbi, &key, nullptr);
                        if (writeCommand.id != TileTransfer::NONE) {
                            app->tileTransfers.at(writeCommand.id).write = TileWriteState::Done;
                        }
                    } break;
                    default:
                        SDL_assert(false && "WriteCommand Type not supported");
                        break;
                }
                prevCanvas = &canvas;
                prevStaged = staged;
            }
            if (txn) {
                ZoneScopedN("Commit txn");
                rc = mdb_txn_commit(txn);
            }
        }
    }
}

} // namespace Midori
