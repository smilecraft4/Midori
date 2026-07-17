#pragma once

#include "base.hpp"
#include "glm/vec2.hpp"
#include "glm/mat4x4.hpp"

namespace Midori {

struct Viewport {
public:
    void Translate(glm::vec2 amount);
    void Zoom(glm::vec2 origin, glm::vec2 amount);
    void Rotate(float amount);
    void FlipHorizontal();
    void FlipVertical();

    void SetTranslation(glm::vec2 newTranslation);
    void SetZoom(glm::vec2 origin, glm::vec2 amount);
    void SetRotation(float newRotation);

    void Resize(glm::vec2 newViewSize);

    void UI() const;

    [[nodiscard]] glm::vec2 Zoom() const;
    [[nodiscard]] glm::mat4 ViewMatrix() const;
    [[nodiscard]] glm::mat4 InverseViewMatrix() const;
    [[nodiscard]] glm::vec2 ScreenToCanvas(glm::vec2 screenPos) const;
    [[nodiscard]] bool IsTileVisible(glm::ivec2 tilePos) const;
    [[nodiscard]] std::vector<glm::ivec2, arena_allocator<glm::ivec2>> VisibleTiles(arena<>& arena) const;

    void ComputeViewMatrix();

    glm::vec2 viewSize{1.0f};

    glm::vec2 translation{0.0f};
    glm::vec2 zoom{1.0f};
    glm::vec2 zoomOrigin{0.0f};
    float rotation{0.0f};
    glm::vec2 flip{1.0f};

    glm::mat4 viewMat{1.0f};
    glm::mat4 viewMatInv{1.0f};
    bool viewComputed{false};

    glm::vec2 vX{1.0f, 0.0f};
    glm::vec2 vY{0.0f, 1.0f};
    glm::vec2 vTrans{0.0f};
};

} // namespace Midori
