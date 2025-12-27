#include "midori/viewport.h"

#include <imgui.h>

#include <numbers>
#include <tracy/Tracy.hpp>

#include "midori/app.h"

namespace Midori {
Viewport::Viewport(App* app) : app_(app) {}

void Viewport::Translate(glm::vec2 amount) {
    // The amount is in view space not canvas space, so we undo every visual modification
    glm::vec2 correctedAmount{};
    correctedAmount.x = amount.x * std::cos(-rotation_) - amount.y * std::sin(-rotation_);
    correctedAmount.y = amount.x * std::sin(-rotation_) + amount.y * std::cos(-rotation_);

    correctedAmount /= zoom_;

    translation_ += correctedAmount;
    view_mat_computed_ = false;
}

void Viewport::SetTranslation(glm::vec2 translation) {
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
    view_mat_computed_ = false;
}

void Viewport::SetZoom(glm::vec2 origin, glm::vec2 amount) {
    // automaticly change the translation based on the zoom origin

    zoom_origin_ = origin;
    zoom_ = amount;
    view_mat_computed_ = false;
}

void Viewport::Rotate(float amount) {
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
    // const glm::ivec2 t_min = glm::floor((-translation_ - (static_cast<glm::vec2>(screenSize) / 2.0f)) / tile_size);
    // const glm::ivec2 t_max = glm::ceil((-translation_ + (static_cast<glm::vec2>(screenSize) / 2.0f)) / tile_size);
    // const glm::ivec2 t_num = t_max - t_min;

    glm::vec2 tMinScreen = glm::floor(-static_cast<glm::vec2>(screenSize) / 2.0f);
    glm::vec2 tMaxScreen = glm::ceil(static_cast<glm::vec2>(screenSize) / 2.0f);

    glm::vec2 tMinCanvas = static_cast<glm::vec2>(InverseViewMatrix() * glm::vec4(tMinScreen, 0.0f, 1.0f)) / tile_size;
    glm::vec2 tMaxCanvas = static_cast<glm::vec2>(InverseViewMatrix() * glm::vec4(tMaxScreen, 0.0f, 1.0f)) / tile_size;

    glm::vec2 xCanvas = static_cast<glm::vec2>(InverseViewMatrix() * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)) / tile_size;
    glm::vec2 yCanvas = static_cast<glm::vec2>(InverseViewMatrix() * glm::vec4(0.0f, 1.0f, 0.0f, 1.0f)) / tile_size;

    std::vector<glm::ivec2> positions;
    // positions.reserve(std::size_t(t_num.x) * std::size_t(t_num.y));

    for (auto y = std::floor(tMinCanvas.y); y < std::ceil(tMaxCanvas.y); y++) {
        for (auto x = std::floor(tMinCanvas.x); x < std::ceil(tMaxCanvas.x); x++) {
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
}  // namespace Midori
void Midori::ViewportCommand::execute() { app_.canvas.viewport = new_viewport_; }

void Midori::ViewportCommand::revert() { app_.canvas.viewport = previous_viewport_; }
