#ifndef MIDORI_RENDERER_H
#define MIDORI_RENDERER_H

#include <unordered_map>

#include "SDL3/SDL_gpu.h"
#include "midori/types.h"

class App;
class Window;

class Renderer {
 public:
  Renderer(const Renderer&) = delete;
  Renderer(Renderer&&) = delete;
  Renderer& operator=(const Renderer&) = delete;
  Renderer& operator=(Renderer&&) = delete;

  explicit Renderer(App* app);
  ~Renderer() = default;

  bool Init(SDL_Window* window);
  bool Render(SDL_Window* window);
  void Quit();

  bool Resize(int width, int height);

  App* app_;

  SDL_GPUDevice* device_;
  SDL_GPUViewport viewport_;

  SDL_GPUGraphicsPipeline* tile_graphics_pipeline_;
  std::unordered_map<Tile, SDL_GPUTexture*> m_tile_textures_;

  SDL_GPUGraphicsPipeline* layer_graphics_pipeline_;
  std::unordered_map<Layer, SDL_GPUTexture*> m_layer_textures_;
};

#endif