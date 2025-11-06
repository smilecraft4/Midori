#ifndef MIDORI_APP_H
#define MIDORI_APP_H

#include <filesystem>

#include "SDL3/SDL_events.h"
#include "SDL3/SDL_video.h"
#include "glm/vec4.hpp"
#include "renderer.h"

constexpr int APP_WINDOW_WIDTH_DEFAULT = 1280;
constexpr int APP_WINDOW_HEIGHT_DEFAULT = 720;

class App {
 public:
  App(const App &) = delete;
  App(App &&) = delete;
  App &operator=(const App &) = delete;
  App &operator=(App &&) = delete;

  App();
  ~App() = default;

  bool Init();
  void Quit();

  bool Update();
  bool Resize(int new_width, int new_height);
  bool KeyPress();
  bool KeyRelease();

  bool New();
  bool SaveAs(const std::filesystem::path &filename);
  bool Open(const std::filesystem::path &filename);
  bool Save();

  // Backend specific
  static bool ProcessSDLEvent(SDL_Event *event);

  int window_width_ = APP_WINDOW_WIDTH_DEFAULT;
  int window_height_ = APP_WINDOW_HEIGHT_DEFAULT;

  SDL_Window *window_;
  Renderer renderer_;

  glm::vec4 bg_color_ = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
};

#endif