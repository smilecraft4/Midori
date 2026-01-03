#include "waypoint.h"

namespace Midori {

Waypoint::Waypoint(std::string name, Viewport viewport)
    : name_(std::move(name)),
      viewport_(viewport),
      creation_(std::chrono::system_clock::now()),
      lastVisit_(std::chrono::system_clock::now()),
      saved_(false) {}

void Waypoint::Rename(std::string name) {
    name_ = std::move(name);
    saved_ = false;
}

}  // namespace Midori
