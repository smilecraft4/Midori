#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

namespace Midori {
    
struct Viewport {
    glm::vec2 translation_ = glm::vec2(0.0f);
    glm::vec2 zoom_ = glm::vec2(1.0f);
    glm::vec2 zoomOrigin_ = glm::vec2(0.0f);
    float rotation_ = 0.0f;
    bool flippedH_ = false;

    glm::mat4 viewMat_ = glm::mat4(1.0f);
    glm::mat4 viewMatInv_ = glm::mat4(1.0f);
    bool viewMatComputed_ = false;

    void Translate(glm::vec2 amount);
    void SetTranslation(glm::vec2 translation);
    void Zoom(glm::vec2 origin, glm::vec2 amount);
    void SetZoom(glm::vec2 origin, glm::vec2 amount);
    void Rotate(float amount);
    void SetRotation(float rotation);
    void FlipHorizontal();

    glm::mat4 ViewMatrix();
    glm::mat4 InverseViewMatrix();
    glm::vec2 ScreenToCanvas(glm::vec2 screenPos, glm::ivec2 screenSize);
    std::vector<glm::ivec2> VisibleTiles(glm::ivec2 screenSize);

    void UI();

   private:
    void ComputeViewMatrix();
};

}  // namespace Midori