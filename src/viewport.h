#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <imgui.h>

namespace Midori {

class Viewport {
public:
    void Translate(glm::vec2 amount);
    void Zoom(glm::vec2 origin, glm::vec2 amount);
    void Rotate(float amount);
    void FlipHorizontal();
    void FlipVertical();

    void SetTranslation(glm::vec2 translation);
    void SetZoom(glm::vec2 origin, glm::vec2 amount);
    void SetRotation(float rotation);

    void Resize(glm::vec2 viewSize);

    glm::vec2 Zoom() const;

    glm::mat4 ViewMatrix() const;
    glm::mat4 InverseViewMatrix() const;
    glm::vec2 ScreenToCanvas(glm::vec2 screenPos) const;

    bool IsTileVisible(glm::ivec2 tilePos) const;
    std::vector<glm::ivec2> VisibleTiles() const;

    void UI();

   private:
    void ComputeViewMatrix();

    glm::vec2 viewSize_{1.0f};
    
    glm::vec2 translation_{0.0f};
    glm::vec2 zoom_{1.0f};
    glm::vec2 zoomOrigin_{0.0f};
    float rotation_{0.0f};
    glm::vec2 flip_{1.0f};

    glm::mat4 viewMat_{1.0f};
    glm::mat4 viewMatInv_{1.0f};
    bool viewComputed_{false};
    
    glm::vec2 vX{1.0f, 0.0f};
    glm::vec2 vY{0.0f, 1.0f};
    glm::vec2 vTrans{0.0f};
};

}  // namespace Midori