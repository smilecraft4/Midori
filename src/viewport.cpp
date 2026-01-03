#include "viewport.h"

#include <numbers>

#include <imgui.h>
#include <tracy/Tracy.hpp>

#include "app.h"

namespace Midori {
void Viewport::Translate(glm::vec2 amount) {
    if (flippedH_) {
        amount.x = -amount.x;
    }
    // The amount is in view space not canvas space, so we undo every visual modification
    glm::vec2 correctedAmount{};
    correctedAmount.x = amount.x * std::cos(-rotation_) - amount.y * std::sin(-rotation_);
    correctedAmount.y = amount.x * std::sin(-rotation_) + amount.y * std::cos(-rotation_);

    correctedAmount /= zoom_;

    translation_ += correctedAmount;
    view_mat_computed_ = false;
}

void Viewport::SetTranslation(glm::vec2 translation) {
    // if (flippedH_) {
    //     translation_.x = -translation_.x;
    // }

    glm::vec2 correctedTranslation{};
    correctedTranslation.x = translation.x * std::cos(-rotation_) - translation.y * std::sin(-rotation_);
    correctedTranslation.y = translation.x * std::sin(-rotation_) + translation.y * std::cos(-rotation_);

    translation_ = correctedTranslation;
    view_mat_computed_ = false;
}

void Viewport::Zoom(glm::vec2 origin, glm::vec2 amount) {
    // automaticly change the translation based on the zoom origin

    origin = static_cast<glm::vec2>(InverseViewMatrix() * glm::vec4(origin, 0.0f, 1.0f));

    zoom_origin_ = origin;
    zoom_ += amount;
    zoom_.x = std::max(0.25f, std::min(2.5f, zoom_.x));
    zoom_.y = std::max(0.25f, std::min(2.5f, zoom_.y));
    view_mat_computed_ = false;
}

void Viewport::SetZoom(glm::vec2 origin, glm::vec2 amount) {
    // automaticly change the translation based on the zoom origin

    zoom_origin_ = origin;
    zoom_ = amount;
    zoom_.x = std::max(0.01f, std::min(100.0f, zoom_.x));
    zoom_.y = std::max(0.01f, std::min(100.0f, zoom_.y));
    view_mat_computed_ = false;
}

void Viewport::Rotate(float amount) {
    if (flippedH_) {
        amount = -amount;
    }
    // The input is in radians
    rotation_ += amount;

    if (rotation_ >= 2.0f * std::numbers::pi_v<float>) {
        rotation_ -= 2.0f * std::numbers::pi_v<float>;
    } else if (rotation_ < -2.0f * std::numbers::pi_v<float>) {
        rotation_ += 2.0f * std::numbers::pi_v<float>;
    }

    view_mat_computed_ = false;
}

void Viewport::SetRotation(float rotation) {
    rotation_ = rotation;
    view_mat_computed_ = false;
}

void Viewport::FlipHorizontal() {
    flippedH_ = !flippedH_;
    view_mat_computed_ = false;
}

void Viewport::ComputeViewMatrix() {
    ZoneScoped;
    view_mat_ = glm::mat4(1.0f);

    if (flippedH_) {
        view_mat_ = glm::scale(view_mat_, glm::vec3(-1.0f, 1.0f, 1.0f));
    }
    view_mat_ = glm::scale(view_mat_, glm::vec3(zoom_, 1.0f));

    view_mat_ = glm::rotate(view_mat_, rotation_, glm::vec3(0.0f, 0.0f, 1.0f));
    view_mat_ = glm::translate(view_mat_, glm::vec3(translation_, 0.0f));

    // TODO: take into account zoom offset;

    view_mat_inv_ = glm::inverse(view_mat_);

    view_mat_computed_ = true;
}

glm::mat4 Viewport::ViewMatrix() {
    if (!view_mat_computed_) {
        ComputeViewMatrix();
    }

    return view_mat_;
}

glm::mat4 Viewport::InverseViewMatrix() {
    if (!view_mat_computed_) {
        ComputeViewMatrix();
    }

    return view_mat_inv_;
}

glm::vec2 Viewport::ScreenToCanvas(glm::vec2 screenPos, glm::ivec2 screenSize) {
    glm::vec2 pos = (screenPos - (glm::vec2(screenSize) / 2.0f));
    pos = static_cast<glm::vec2>(InverseViewMatrix() * glm::vec4(pos, 0.0f, 1.0f));
    return pos;
}

std::vector<glm::ivec2> Viewport::VisibleTiles(glm::ivec2 screenSize) {
    ZoneScoped;
    constexpr auto tile_size = glm::vec2(TILE_SIZE);

    glm::vec2 a = static_cast<glm::vec2>(screenSize * glm::ivec2(-1, -1)) / 2.0f;
    glm::vec2 b = static_cast<glm::vec2>(screenSize * glm::ivec2(1, -1)) / 2.0f;
    glm::vec2 c = static_cast<glm::vec2>(screenSize * glm::ivec2(-1, 1)) / 2.0f;
    glm::vec2 d = static_cast<glm::vec2>(screenSize * glm::ivec2(1, 1)) / 2.0f;

    a = static_cast<glm::vec2>(InverseViewMatrix() * glm::vec4(a, 0.0f, 1.0f)) / tile_size;
    b = static_cast<glm::vec2>(InverseViewMatrix() * glm::vec4(b, 0.0f, 1.0f)) / tile_size;
    c = static_cast<glm::vec2>(InverseViewMatrix() * glm::vec4(c, 0.0f, 1.0f)) / tile_size;
    d = static_cast<glm::vec2>(InverseViewMatrix() * glm::vec4(d, 0.0f, 1.0f)) / tile_size;

    glm::vec2 min, max;
    min.x = std::floor(std::min(a.x, std::min(b.x, std::min(c.x, d.x))));
    min.y = std::floor(std::min(a.y, std::min(b.y, std::min(c.y, d.y))));
    max.x = std::ceil(std::max(a.x, std::max(b.x, std::max(c.x, d.x))));
    max.y = std::ceil(std::max(a.y, std::max(b.y, std::max(c.y, d.y))));

    std::vector<glm::ivec2> positions;
    for (int y = min.y; y < max.y; y++) {
        for (int x = min.x; x < max.x; x++) {
            // TODO: make sure the position is inside the rectangle

            positions.emplace_back(x, y);
        }
    }

    return positions;
}

void Viewport::UI() {
    if (ImGui::Begin("Viewport")) {
        ImGui::LabelText("Pos", "%.2f, %.2f", translation_.x, translation_.y);
        ImGui::LabelText("Zoom", "%.2f, %.2f", zoom_.x, zoom_.y);
        ImGui::LabelText("Zoom origin", "%.2f, %.2f", zoom_origin_.x, zoom_origin_.y);
        ImGui::LabelText("Rotation", "%.2f°", rotation_);
        ImGui::LabelText("Flipped H", "%s", flippedH_ ? "true" : "false");
    }
    ImGui::End();
}
void ViewportChangeCommand::execute() { app_.canvas.viewport = new_viewport_; }

void ViewportChangeCommand::revert() { app_.canvas.viewport = previous_viewport_; }

void ViewportChangeCommand::SetNewViewport(Viewport viewport) { new_viewport_ = viewport; }
void ViewportChangeCommand::SetPreviousViewport(Viewport viewport) { previous_viewport_ = viewport; }

}  // namespace Midori