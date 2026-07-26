#include "viewport.hpp"

#include "defines.hpp"

#include <SDL3/SDL_assert.h>
#include <glm/ext/matrix_transform.hpp>
#include <imgui.h>
#include <numbers>
#include <tracy/Tracy.hpp>

namespace Midori {

void Viewport::Translate(glm::vec2 amount) {
    amount *= flip;
    glm::vec2 correctedAmount{};
    correctedAmount.x = (amount.x * std::cos(-rotation)) - (amount.y * std::sin(-rotation));
    correctedAmount.y = (amount.x * std::sin(-rotation)) + (amount.y * std::cos(-rotation));
    correctedAmount /= zoom;
    translation += correctedAmount;

    viewComputed = false;
    ComputeViewMatrix();
}

void Viewport::SetTranslation(glm::vec2 newTranslation) {
    glm::vec2 correctedTranslation{};
    correctedTranslation.x = (newTranslation.x * std::cos(-rotation)) - (newTranslation.y * std::sin(-rotation));
    correctedTranslation.y = (newTranslation.x * std::sin(-rotation)) + (newTranslation.y * std::cos(-rotation));
    translation = correctedTranslation;

    viewComputed = false;
    ComputeViewMatrix();
}

void Viewport::Zoom(glm::vec2 origin, glm::vec2 amount) {
    // const glm::vec2 pivotCanvas = static_cast<glm::vec2>(InverseViewMatrix() * glm::vec4(origin, 0.0f, 1.0f));
    // zoomOrigin = pivotCanvas;

    zoom.x *= std::exp(amount.x);
    zoom.y *= std::exp(amount.y);

    zoom.x = std::max(VIEWPORT_ZOOM_MIN, std::min(VIEWPORT_ZOOM_MAX, zoom.x));
    zoom.y = std::max(VIEWPORT_ZOOM_MIN, std::min(VIEWPORT_ZOOM_MAX, zoom.y));

    viewComputed = false;
    ComputeViewMatrix();

    // const glm::vec2 pivotScreen = static_cast<glm::vec2>(ViewMatrix() * glm::vec4(pivotCanvas, 0.0f, 1.0f));
    // Translate(origin - pivotScreen);
}

void Viewport::SetZoom(glm::vec2 newZoom) {
    zoom = newZoom;
    zoom.x = std::max(VIEWPORT_ZOOM_MIN, std::min(VIEWPORT_ZOOM_MAX, zoom.x));
    zoom.y = std::max(VIEWPORT_ZOOM_MIN, std::min(VIEWPORT_ZOOM_MAX, zoom.y));

    viewComputed = false;
    ComputeViewMatrix();
}

void Viewport::Rotate(float amount) {
    amount *= flip.x;
    rotation += amount;

    if (rotation >= 2.0f * std::numbers::pi_v<float>) {
        rotation -= 2.0f * std::numbers::pi_v<float>;
    } else if (rotation < -2.0f * std::numbers::pi_v<float>) {
        rotation += 2.0f * std::numbers::pi_v<float>;
    }

    viewComputed = false;
    ComputeViewMatrix();
}

void Viewport::SetRotation(float newRotation) {
    rotation = newRotation;
    viewComputed = false;
    ComputeViewMatrix();
}

void Viewport::Resize(glm::vec2 newViewSize) {
    viewSize = newViewSize;
    viewComputed = false;
    ComputeViewMatrix();
}

void Viewport::FlipHorizontal() {
    flip.x = -flip.x;
    viewComputed = false;
    ComputeViewMatrix();
}

void Viewport::FlipVertical() {
    flip.y = -flip.y;
    viewComputed = false;
    ComputeViewMatrix();
}

glm::vec2 Viewport::Zoom() const {
    return zoom;
}

void Viewport::ComputeViewMatrix() {
    ZoneScoped;
    viewMat = glm::mat4(1.0f);
    viewMat = glm::scale(viewMat, glm::vec3(flip.x, flip.y, 1.0f));
    viewMat = glm::scale(viewMat, glm::vec3(zoom, 1.0f));
    viewMat = glm::rotate(viewMat, rotation, glm::vec3(0.0f, 0.0f, 1.0f));
    viewMat = glm::translate(viewMat, glm::vec3(glm::floor(translation), 0.0f)); // Snap to pixels

    vX = glm::vec2(viewMat[0]) * static_cast<float>(TILE_WIDTH);
    vY = glm::vec2(viewMat[1]) * static_cast<float>(TILE_HEIGHT);
    vTrans = glm::vec2(viewMat[3]);

    viewMatInv = glm::inverse(viewMat);

    viewComputed = true;
}

glm::mat4 Viewport::ViewMatrix() const {
    SDL_assert(viewComputed);
    return viewMat;
}

glm::mat4 Viewport::InverseViewMatrix() const {
    SDL_assert(viewComputed);
    return viewMatInv;
}

glm::vec2 Viewport::ScreenToCanvas(glm::vec2 screenPos) const {
    SDL_assert(viewComputed);
    glm::vec2 pos = screenPos - (viewSize / 2.0f);
    pos = static_cast<glm::vec2>(InverseViewMatrix() * glm::vec4(pos, 0.0f, 1.0f));
    return pos;
}

bool Viewport::IsTileVisible(glm::ivec2 tilePos) const {
    SDL_assert(viewComputed);
    const glm::vec2 t0 = static_cast<float>(tilePos.x) * vX + static_cast<float>(tilePos.y) * vY + vTrans;
    const glm::vec2 tCorners[4] = {
        t0,
        t0 + vX,
        t0 + vY,
        t0 + vX + vY,
    };

    glm::vec2 tMin{tCorners[0]};
    glm::vec2 tMax{tCorners[0]};
    for (int i = 1; i < 4; ++i) {
        tMin = glm::min(tMin, tCorners[i]);
        tMax = glm::max(tMax, tCorners[i]);
    }

    return (tMin.x <= (viewSize.x / 2.0f) && tMax.x >= (-viewSize.x / 2.0f))
        && (tMin.y <= (viewSize.y / 2.0f) && tMax.y >= (-viewSize.y / 2.0f));
}

std::vector<glm::ivec2, arena_allocator<glm::ivec2>> Viewport::VisibleTiles(arena<>& arena) const {
    SDL_assert(viewComputed);
    constexpr glm::vec2 T_SIZE(TILE_WIDTH, TILE_HEIGHT);

    // Broad pass (AABB in canvas Space)
    const glm::vec2 vCorners[4] = {
        glm::vec2(viewMatInv * glm::vec4(viewSize * glm::vec2(-0.5f, -0.5f), 0.0f, 1.0f)),
        glm::vec2(viewMatInv * glm::vec4(viewSize * glm::vec2(0.5f, -0.5f), 0.0f, 1.0f)),
        glm::vec2(viewMatInv * glm::vec4(viewSize * glm::vec2(-0.5f, 0.5f), 0.0f, 1.0f)),
        glm::vec2(viewMatInv * glm::vec4(viewSize * glm::vec2(0.5f, 0.5f), 0.0f, 1.0f)),
    };

    glm::vec2 vAabbMin{vCorners[0]};
    glm::vec2 vAabbMax{vCorners[0]};
    for (int i = 1; i < 4; ++i) {
        vAabbMin = glm::min(vAabbMin, vCorners[i]);
        vAabbMax = glm::max(vAabbMax, vCorners[i]);
    }

    vAabbMin = glm::floor(vAabbMin / T_SIZE);
    vAabbMax = glm::ceil(vAabbMax / T_SIZE);

    std::vector<glm::ivec2, arena_allocator<glm::ivec2>> tPositions(arena);
    tPositions.reserve(static_cast<size_t>(vAabbMax.x - vAabbMin.x) * static_cast<size_t>(vAabbMax.y - vAabbMin.y));

    // If needed this can be simded
    for (int y = std::floor(vAabbMin.y); y < static_cast<int>(std::ceil(vAabbMax.y)); y++) {
        for (int x = std::floor(vAabbMin.x); x < static_cast<int>(std::ceil(vAabbMax.x)); x++) {
            if (IsTileVisible(glm::ivec2(x, y))) {
                tPositions.emplace_back(x, y);
            }
        }
    }

    return tPositions;
}

void Viewport::UI() const {
    if (ImGui::Begin("Viewport")) {
        ImGui::LabelText("Pos", "%.2f, %.2f", translation.x, translation.y);
        ImGui::LabelText("Zoom", "%.2f, %.2f", zoom.x, zoom.y);
        ImGui::LabelText("Zoom origin", "%.2f, %.2f", zoomOrigin.x, zoomOrigin.y);
        ImGui::LabelText("Rotation", "%.2f", rotation);
        ImGui::LabelText("Flipped", "(%.1f, %.1f)", flip.x, flip.y);
    }
    ImGui::End();
}

} // namespace Midori
