#pragma once

#include <unordered_set>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

#include "command.h"

namespace Midori {
class App;

struct Viewport {
    glm::vec2 translation_ = glm::vec2(0.0f);
    glm::vec2 zoom_ = glm::vec2(1.0f);
    glm::vec2 zoom_origin_ = glm::vec2(0.0f);
    float rotation_ = 0.0f;
    bool flippedH_ = false;

    glm::mat4 view_mat_ = glm::mat4(1.0f);
    glm::mat4 view_mat_inv_ = glm::mat4(1.0f);
    bool view_mat_computed_ = false;

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

class ViewportChangeCommand : public Command {
   private:
    App& app_;
    Viewport new_viewport_;
    Viewport previous_viewport_;

   public:
    ViewportChangeCommand(App& app) : Command(Type::Unknown), app_(app) {};
    virtual ~ViewportChangeCommand() = default;

    virtual std::string name() const { return "View change"; }
    virtual void execute();
    virtual void revert();

    void SetNewViewport(Viewport viewport);
    void SetPreviousViewport(Viewport viewport);
};

}  // namespace Midori