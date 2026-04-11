#include "viewport.h"

#include "app.h"
#include "commands.h"
#include <imgui.h>
#include <numbers>
#include <tracy/Tracy.hpp>

namespace Midori {
void Viewport::Translate(glm::vec2 amount) {
    amount *= flip_;
    glm::vec2 correctedAmount{};
    correctedAmount.x = amount.x * std::cos(-rotation_) - amount.y * std::sin(-rotation_);
    correctedAmount.y = amount.x * std::sin(-rotation_) + amount.y * std::cos(-rotation_);
    correctedAmount /= zoom_;
    translation_ += correctedAmount;

    viewComputed_ = false;
    ComputeViewMatrix();
}

void Viewport::SetTranslation(glm::vec2 translation) {
    glm::vec2 correctedTranslation{};
    correctedTranslation.x = translation.x * std::cos(-rotation_) - translation.y * std::sin(-rotation_);
    correctedTranslation.y = translation.x * std::sin(-rotation_) + translation.y * std::cos(-rotation_);
    translation_ = correctedTranslation;

    viewComputed_ = false;
    ComputeViewMatrix();
}

void Viewport::Zoom(glm::vec2 origin, glm::vec2 amount) {
    origin = static_cast<glm::vec2>(InverseViewMatrix() * glm::vec4(origin, 0.0f, 1.0f));

    zoomOrigin_ = origin;
    zoom_ += amount;
    zoom_.x = std::max(0.25f, std::min(2.5f, zoom_.x));
    zoom_.y = std::max(0.25f, std::min(2.5f, zoom_.y));

    viewComputed_ = false;
    ComputeViewMatrix();
}

void Viewport::SetZoom(glm::vec2 origin, glm::vec2 amount) {
    zoomOrigin_ = origin;
    zoom_ = amount;
    zoom_.x = std::max(0.01f, std::min(100.0f, zoom_.x));
    zoom_.y = std::max(0.01f, std::min(100.0f, zoom_.y));

    viewComputed_ = false;
    ComputeViewMatrix();
}

void Viewport::Rotate(float amount) {    
    amount *= flip_.x;
    rotation_ += amount;

    if (rotation_ >= 2.0f * std::numbers::pi_v<float>) {
        rotation_ -= 2.0f * std::numbers::pi_v<float>;
    } else if (rotation_ < -2.0f * std::numbers::pi_v<float>) {
        rotation_ += 2.0f * std::numbers::pi_v<float>;
    }

    viewComputed_ = false;
    ComputeViewMatrix();
}

void Viewport::SetRotation(float rotation) {
    rotation_ = rotation;
    viewComputed_ = false;
    ComputeViewMatrix();
}

void Viewport::Resize(glm::vec2 viewSize) {
    viewSize_ = viewSize;
    viewComputed_ = false;
    ComputeViewMatrix();
}

void Viewport::FlipHorizontal() {
    flip_.x = -flip_.x;
    viewComputed_ = false;
    ComputeViewMatrix();
}

void Viewport::FlipVertical() {
    flip_.y = -flip_.y;
    viewComputed_ = false;
    ComputeViewMatrix();
}

glm::vec2 Viewport::Zoom() const {
    return zoom_;
}

void Viewport::ComputeViewMatrix() {
    ZoneScoped;
    viewMat_ = glm::mat4(1.0f);
    viewMat_ = glm::scale(viewMat_, glm::vec3(flip_.x, flip_.y, 1.0f));
    viewMat_ = glm::scale(viewMat_, glm::vec3(zoom_, 1.0f));
    viewMat_ = glm::rotate(viewMat_, rotation_, glm::vec3(0.0f, 0.0f, 1.0f));
    viewMat_ = glm::translate(viewMat_, glm::vec3(glm::floor(translation_), 0.0f)); // Snap to pixels

    vX = glm::vec2(viewMat_[0]) * static_cast<float>(TILE_WIDTH);
    vY = glm::vec2(viewMat_[1]) * static_cast<float>(TILE_HEIGHT);
    vTrans = glm::vec2(viewMat_[3]);

    // TODO: take into account zoom offset;

    viewMatInv_ = glm::inverse(viewMat_);

    viewComputed_ = true;
}

glm::mat4 Viewport::ViewMatrix() const {
    SDL_assert(viewComputed_);
    return viewMat_;
}

glm::mat4 Viewport::InverseViewMatrix() const {
    SDL_assert(viewComputed_);
    return viewMatInv_;
}

glm::vec2 Viewport::ScreenToCanvas(glm::vec2 screenPos) const {
    SDL_assert(viewComputed_);
    glm::vec2 pos = screenPos - (viewSize_ / 2.0f);
    pos = static_cast<glm::vec2>(InverseViewMatrix() * glm::vec4(pos, 0.0f, 1.0f));
    return pos;
}

bool Viewport::IsTileVisible(glm::ivec2 tilePos) const {
    SDL_assert(viewComputed_);
    const glm::vec2 t0 = static_cast<float>(tilePos.x) * vX + static_cast<float>(tilePos.y) * vY + vTrans;
    const std::array<glm::vec2, 4> tCorners = {
        t0,
        t0 + vX,
        t0 + vY,
        t0 + vX + vY,
    };

    glm::vec2 tMin{tCorners[0]}, tMax{tCorners[0]};
    for (int i = 1; i < 4; ++i) {
        tMin = glm::min(tMin, tCorners[i]);
        tMax = glm::max(tMax, tCorners[i]);
    }

    if ((tMin.x <= (viewSize_.x / 2.0f) && tMax.x >= (-viewSize_.x / 2.0f)) &&
        (tMin.y <= (viewSize_.y / 2.0f) && tMax.y >= (-viewSize_.y / 2.0f))) {
        return true;
    }

    return false;
}

std::vector<glm::ivec2> Viewport::VisibleTiles() const {
    SDL_assert(viewComputed_);
    constexpr glm::vec2 tSize(TILE_WIDTH, TILE_HEIGHT);

    // Broad pass (AABB in canvas Space)
    const glm::vec2 vCorners[4] = {
        glm::vec2(viewMatInv_ * glm::vec4(viewSize_ * glm::vec2(-0.5f, -0.5f), 0.0f, 1.0f)),
        glm::vec2(viewMatInv_ * glm::vec4(viewSize_ * glm::vec2(0.5f, -0.5f), 0.0f, 1.0f)),
        glm::vec2(viewMatInv_ * glm::vec4(viewSize_ * glm::vec2(-0.5f, 0.5f), 0.0f, 1.0f)),
        glm::vec2(viewMatInv_ * glm::vec4(viewSize_ * glm::vec2(0.5f, 0.5f), 0.0f, 1.0f)),
    };

    glm::vec2 vAabbMin{vCorners[0]}, vAabbMax{vCorners[0]};
    for (int i = 1; i < 4; ++i) {
        vAabbMin = glm::min(vAabbMin, vCorners[i]);
        vAabbMax = glm::max(vAabbMax, vCorners[i]);
    }

    vAabbMin = glm::floor(vAabbMin / tSize);
    vAabbMax = glm::ceil(vAabbMax / tSize);

    std::vector<glm::ivec2> tPositions;
    tPositions.reserve(((vAabbMax.x) - std::floor(vAabbMin.x)) *
                       (std::ceil(vAabbMax.y) - std::floor(vAabbMin.y)));

    // If needed this can be simded
    for (int y = std::floor(vAabbMin.y); y < std::ceil(vAabbMax.y); y++) {
        for (int x = std::floor(vAabbMin.x); x < std::ceil(vAabbMax.x); x++) {
            if (IsTileVisible(glm::ivec2(x, y))) {
                tPositions.push_back(glm::ivec2(x, y));
            }
        }
    }

    return tPositions;
}

void Viewport::UI() {
    if (ImGui::Begin("Viewport")) {
        ImGui::LabelText("Pos", "%.2f, %.2f", translation_.x, translation_.y);
        ImGui::LabelText("Zoom", "%.2f, %.2f", zoom_.x, zoom_.y);
        ImGui::LabelText("Zoom origin", "%.2f, %.2f", zoomOrigin_.x, zoomOrigin_.y);
        ImGui::LabelText("Rotation", "%.2f", rotation_);
        ImGui::LabelText("Flipped", "(%.1f, %.1f)", flip_.x, flip_.y);
    }
    ImGui::End();
}

} // namespace Midori