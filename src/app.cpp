#include "midori/app.h"
#include "SDL3/SDL_log.h"

#include <format>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <tracy/Tracy.hpp>

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
                            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN |
                                SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (window == nullptr) {
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to create window: %s",
                    SDL_GetError());
    return false;
  }
  SDL_SetWindowPosition(window, window_pos.x, window_pos.y);
  SDL_ShowWindow(window);

  SDL_GetMouseState(&cursor_current_pos.x, &cursor_current_pos.y);
  cursor_last_pos = cursor_current_pos;

  if (!renderer.Init()) {
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to initialize renderer");
    return false;
  }

  if (!canvas.New()) {
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to create new canvas");
    return false;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  ImGui::StyleColorsLight();

  ImGuiStyle &style = ImGui::GetStyle();
  style.ScaleAllSizes(main_scale);
  style.FontScaleDpi = main_scale;
  // TODO: load cascadia font

  ImGui_ImplSDL3_InitForSDLGPU(window);
  ImGui_ImplSDLGPU3_InitInfo init_info = {
      .Device = renderer.device,
      .ColorTargetFormat =
          SDL_GetGPUSwapchainTextureFormat(renderer.device, window),
      .MSAASamples = SDL_GPU_SAMPLECOUNT_1,
      .SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
      .PresentMode = SDL_GPU_PRESENTMODE_VSYNC,
  };
  ImGui_ImplSDLGPU3_Init(&init_info);

  return true;
}

void DebugDrawTile(ImDrawList *draw_list, glm::ivec2 pos) {}

static const SDL_DialogFileFilter filters[] = {{"Midori files (.mido)", "mido"},
                                               {"All files", "*"}};

bool App::Update() {
  FrameMark;
  if (window_size.x == 0 || window_size.y == 0) {
    return true;
  }

  { // ImGui Stuff
    ZoneScopedN("UI");
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    if (ImGui::Begin("File")) {
      if (ImGui::Button("New")) {
        SDL_WaitForGPUIdle(renderer.device);
        canvas.New();
      }
      if (ImGui::Button("SaveAs")) {
        SDL_WaitForGPUIdle(renderer.device);
        SDL_ShowSaveFileDialog(
            [](void *userdata, const char *const *filelist, int filter) {
              App *app = (App *)(userdata);

              if (!filelist) {
                SDL_Log("An error occured: %s", SDL_GetError());
                return;
              } else if (!*filelist) {
                SDL_Log("The user did not select any file.");
                SDL_Log("Most likely, the dialog was canceled.");
                return;
              }

              SDL_Log("Full path to selected file: '%s'", *filelist);
              if (!app->canvas.SaveAs(*filelist)) {
                SDL_Log("Failed to saved file as: '%s'", *filelist);
              }
            },
            this, window, filters, SDL_arraysize(filters), nullptr);
      }
      if (ImGui::Button("Open")) {
        SDL_WaitForGPUIdle(renderer.device);
        SDL_ShowOpenFileDialog(
            [](void *userdata, const char *const *filelist, int filter) {
              App *app = (App *)(userdata);

              if (!filelist) {
                SDL_Log("An error occured: %s", SDL_GetError());
                return;
              } else if (!*filelist) {
                SDL_Log("The user did not select any file.");
                SDL_Log("Most likely, the dialog was canceled.");
                return;
              }

              SDL_Log("Full path to selected file: '%s'", *filelist);
              if (!app->canvas.Open(*filelist)) {
                SDL_Log("Failed to saved file as: '%s'", *filelist);
              }
            },
            this, window, filters, SDL_arraysize(filters), nullptr, false);
      }
      if (ImGui::Button("Save")) {
        SDL_WaitForGPUIdle(renderer.device);
        canvas.Save();
      }
    }
    ImGui::End();

    /*
    static bool ui_debug_culling = false;
    if (ImGui::Begin("View")) {
      ImGui::LabelText("Position", "x:%.2f y:%.2f", canvas.view.pan.x,
                       canvas.view.pan.y);
      ImGui::LabelText("Zoom", "%.2f", canvas.view.zoom_amount);
      ImGui::LabelText("Rotation", "%.2f",
                       canvas.view.rotation / std::numbers::pi * 360.0f);
      ImGui::Separator();
      ImGui::Checkbox("Debug culling", &ui_debug_culling);
    }
    ImGui::End();
    if (ui_debug_culling) {
      const std::vector<glm::ivec2> visible_tiles_positions =
          canvas.GetVisibleTilePositions(canvas.view, window_size / 2);
      ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
      {
        const ImVec2 pmin = {
            ((float)window_size.x / 2.0f) + ((float)window_size.x / -4.0f),
            ((float)window_size.y / 2.0f) + ((float)window_size.y / -4.0f),
        };
        const ImVec2 pmax = {
            pmin.x + ((float)window_size.x / 2.0f),
            pmin.y + ((float)window_size.y / 2.0f),
        };
        draw_list->AddRect(pmin, pmax, IM_COL32(255, 0, 255, 255));
      }
      for (const auto &pos : visible_tiles_positions) {
        const ImVec2 pmin = {
            ((float)window_size.x / 2.0f) + canvas.view.pan.x +
                ((float)pos.x * (float)TILE_SIZE),
            ((float)window_size.y / 2.0f) + canvas.view.pan.y +
                ((float)pos.y * (float)TILE_SIZE),
        };
        const ImVec2 pmax = {
            pmin.x + (float)TILE_SIZE,
            pmin.y + (float)TILE_SIZE,
        };
        const ImVec2 pcenter = {
            pmin.x + (float)TILE_SIZE / 2.0f,
            pmin.y + (float)TILE_SIZE / 2.0f,
        };

        draw_list->AddRect(pmin, pmax, IM_COL32(255, 255, 255, 255));
        std::string text = std::format("[{}, {}]", pos.x, pos.y);
        draw_list->AddText(pcenter, IM_COL32(255, 255, 255, 128), text.c_str());
      }
    }
    */
    {
      ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
      const glm::ivec2 pos = glm::floor((cursor_current_pos - canvas.view.pan -
                                         (glm::vec2(window_size) / 2.0f)) /
                                        glm::vec2(TILE_SIZE));
      const ImVec2 pmin = {
          ((float)window_size.x / 2.0f) + canvas.view.pan.x +
              ((float)pos.x * (float)TILE_SIZE),
          ((float)window_size.y / 2.0f) + canvas.view.pan.y +
              ((float)pos.y * (float)TILE_SIZE),
      };
      const ImVec2 pmax = {
          pmin.x + (float)TILE_SIZE,
          pmin.y + (float)TILE_SIZE,
      };
      const ImVec2 pcenter = {
          pmin.x + (float)TILE_SIZE / 2.0f,
          pmin.y + (float)TILE_SIZE / 2.0f,
      };

      draw_list->AddRect(pmin, pmax, IM_COL32(255, 0, 0, 255));
      std::string text = std::format("[{}, {}]", pos.x, pos.y);
      draw_list->AddText(pcenter, IM_COL32(255, 0, 0, 128), text.c_str());
    }

    if (ImGui::Begin("Tile infos")) {
      ImGui::LabelText("Total tiles", "%llu", canvas.tile_infos.size());
      ImGui::LabelText("Queued Tiles", "%llu", canvas.tile_load_queue.size());
      ImGui::LabelText("Tile texture", "%llu", renderer.tile_textures.size());
      ImGui::LabelText("Tile uploading allocated", "%llu",
                       renderer.allocated_tile_upload_offset.size());
      ImGui::LabelText("Tile Last rendered", "%llu",
                       renderer.last_rendered_tiles_num);
    }
    ImGui::End();

    if (ImGui::Begin("Layers")) {
      // Temporary layer stuff
      std::vector<LayerInfo> layer_info;
      layer_info.reserve(canvas.layer_infos.size());
      for (auto &[layer, info] : canvas.layer_infos) {
        layer_info.push_back(info);
      }

      std::ranges::sort(layer_info, [](LayerInfo &a, LayerInfo &b) {
        return a.depth > b.depth;
      });

      const ImVec2 image_size = {
          window_size.x / (float)window_size.y * 50.0f,
          50.0f,
      };
      for (int i = layer_info.size() - 1; i >= 0; i--) {
        auto &real_data = canvas.layer_infos[layer_info[i].layer];
        const std::string title =
            std::format("-{}: {}", real_data.depth, real_data.name);
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
        canvas.CreateLayer("New Layer", 0);
      }
    }
    ImGui::ColorEdit4("Background Color", glm::value_ptr(bg_color));
    ImGui::End();

    // TODO: Debug tile multithreaded download process (GPU->CPU->Disk)
    // TODO: Debug tile multithreaded upload process (Disk->CPU->GPU)

    if (ImGui::Begin("Stroke Options")) {
      ImGui::ColorEdit4("Color", glm::value_ptr(canvas.stroke_options.color));
      ImGui::SliderFloat("Radius", &canvas.stroke_options.radius, 0.1f, 100.0f);
      ImGui::SliderFloat("Flow", &canvas.stroke_options.flow, 0.0f, 1.0f);
      ImGui::SliderFloat("Hardness", &canvas.stroke_options.hardness, 0.0f,
                         1.0f);
      ImGui::SliderFloat("Spacing", &canvas.stroke_options.spacing, 0.1f,
                         10.0f);
      ImGui::Separator();
      ImGui::LabelText("Stroke num", "%llu", canvas.stroke_points.size());
    }
    ImGui::End();

    ImGui::Render();
  }

  canvas.Update();

  if (!renderer.Render()) {
    return false;
  }

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

void App::Quit() {
  ZoneScoped;
  SDL_WaitForGPUIdle(renderer.device);
  ImGui_ImplSDL3_Shutdown();
  ImGui_ImplSDLGPU3_Shutdown();
  ImGui::DestroyContext();

  canvas.Quit();
  renderer.Quit();

  SDL_DestroyWindow(window);
  window = nullptr;
}

void App::CursorMove(glm::vec2 new_pos) {
  cursor_last_pos = cursor_current_pos;
  cursor_current_pos = new_pos;
  cursor_delta_pos = cursor_current_pos - cursor_last_pos;

  if (cursor_left_pressed) {
    canvas.ViewUpdateCursor(cursor_current_pos, cursor_delta_pos);

    if (!canvas.view_panning && !canvas.view_zooming && !canvas.view_rotating &&
        (canvas.selected_layer != 0)) {
      const glm::ivec2 pos = (cursor_current_pos - canvas.view.pan -
                              (glm::vec2(window_size) / 2.0f));

      canvas.UpdateStroke(Canvas::StrokePoint{
          .position = pos,
      });
    }
  }

  if (cursor_right_pressed) {
    if (!canvas.view_panning && !canvas.view_zooming && !canvas.view_rotating &&
        (canvas.selected_layer != 0)) {
      const glm::ivec2 pos = glm::floor((cursor_current_pos - canvas.view.pan -
                                         (glm::vec2(window_size) / 2.0f)) /
                                        glm::vec2(TILE_SIZE));
      canvas.DeleteTile(canvas.selected_layer, pos);

      // TODO: Create proper save,delete tile
      canvas.file.layers.at(canvas.selected_layer).tile_saved.erase(pos);
      canvas.file.saved = false;
    }
  }

  cursor_delta_pos = glm::vec2(0.0f);
}
void App::CursorPress(Uint8 button) {
  if (button == SDL_BUTTON_LEFT) {
    cursor_left_pressed = true;

    if (!canvas.view_panning && !canvas.view_zooming && !canvas.view_rotating &&
        (canvas.selected_layer != 0)) {
      const glm::ivec2 pos = (cursor_current_pos - canvas.view.pan -
                              (glm::vec2(window_size) / 2.0f));

      canvas.StartStroke(Canvas::StrokePoint{
          .position = pos,
      });
    }
  }
  if (button == SDL_BUTTON_RIGHT) {
    cursor_right_pressed = true;

    if (!canvas.view_panning && !canvas.view_zooming && !canvas.view_rotating &&
        (canvas.selected_layer != 0)) {
      const glm::ivec2 pos = glm::floor((cursor_current_pos - canvas.view.pan -
                                         (glm::vec2(window_size) / 2.0f)) /
                                        glm::vec2(TILE_SIZE));
      canvas.DeleteTile(canvas.selected_layer, pos);

      // TODO: Create proper save,delete tile
      canvas.file.layers.at(canvas.selected_layer).tile_saved.erase(pos);
      canvas.file.saved = false;
    }
  }
}
void App::CursorRelease(Uint8 button) {
  if (button == SDL_BUTTON_LEFT) {
    cursor_left_pressed = false;
    if (!canvas.view_panning && !canvas.view_zooming && !canvas.view_rotating &&
        (canvas.selected_layer != 0)) {
      const glm::ivec2 pos = (cursor_current_pos - canvas.view.pan -
                              (glm::vec2(window_size) / 2.0f));

      canvas.EndStroke(Canvas::StrokePoint{
          .position = pos,
      });
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

  canvas.ViewUpdateState(space_pressed, false, false);
}

void App::KeyRelease(SDL_Keycode key, SDL_Keymod mods) {
  if (key == SDLK_SPACE) {
    space_pressed = false;
  }
  if (key == SDLK_LCTRL || key == SDLK_RCTRL) {
    ctrl_pressed = false; // FIXME: How to detect when both of them are pressed
  }
  if (key == SDLK_LSHIFT || key == SDLK_RSHIFT) {
    shift_pressed = false; // FIXME: How to detect when both of them are pressed
  }

  canvas.ViewUpdateState(space_pressed, false, false);
}

} // namespace Midori