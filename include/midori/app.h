#ifndef MIDORI_APP_H
#define MIDORI_APP_H

#include <SDL3/SDL.h>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <string>
#include <vector>

#include "midori/canvas.h"
#include "midori/renderer.h"

namespace Midori {

class App {
public:
  App(const App &) = delete;
  App(App &&) = delete;
  App &operator=(const App &) = delete;
  App &operator=(App &&) = delete;

  App(int argc, char *argv[]);
  ~App() = default;

  bool Init();
  bool Update();
  bool Resize(int width, int height);
  void Quit();

  SDL_Window *window;
  std::vector<std::string> args;

  Canvas canvas;
  Renderer renderer;

  glm::vec4 bg_color = {1.0F, 1.0F, 1.0F, 1.0F};
  glm::ivec2 window_pos = {SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED};
  glm::ivec2 window_size = {1280, 720};
};
} // namespace Midori

#endif
