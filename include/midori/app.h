#ifndef MIDORI_APP_H
#define MIDORI_APP_H

#include <SDL3/SDL.h>
#include <glm/vec4.hpp>
#include <string>

#include "midori/array.h"

namespace Midori {
class App {
public:
  App(int argc, char *argv[]);

  bool Init();
  bool Render();
  void Quit();

  SDL_Window *window;
  SDL_GPUDevice *gpu_device;
  Array<std::string> args;

  glm::vec4 bg_color = {1.0F, 1.0F, 1.0F, 1.0F};
};
} // namespace Midori

#endif