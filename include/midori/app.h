#ifndef MIDORI_APP_H
#define MIDORI_APP_H

#include <SDL3/SDL.h>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <string>
#include <vector>

namespace Midori {

class App {
public:
  App(int argc, char *argv[]);

  bool Init();
  bool Update();
  bool Render();
  bool Resize(int width, int height);
  void Quit();

  SDL_Window *window;
  SDL_GPUDevice *gpu_device;
  std::vector<std::string> args;

  glm::vec4 bg_color = {1.0F, 1.0F, 1.0F, 1.0F};
  glm::ivec2 window_pos = {SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED};
  glm::ivec2 window_size = {1280, 720};
};
} // namespace Midori

#endif