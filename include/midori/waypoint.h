#pragma once
#include <chrono>
#include <string>

#include "midori/command.h"
#include "midori/viewport.h"

namespace Midori {
struct Waypoint {
    Waypoint(std::string name, Viewport viewport);
    void Rename(std::string name);

    Viewport viewport_;
    std::chrono::time_point<std::chrono::system_clock> creation_;
    std::chrono::time_point<std::chrono::system_clock> lastVisit_;
    std::string name_;

    bool saved_ = false;
};

class WaypointCreateCommand : public Command {
   private:
    App& app_;

   public:
    WaypointCreateCommand(App& app) : Command(Type::Unknown), app_(app) {};
    virtual ~WaypointCreateCommand() = default;

    virtual std::string name() const { return "Waypoint Create"; }
    virtual void execute() {};
    virtual void revert() {};
};

class WaypointDeleteCommand : public Command {
   private:
    App& app_;

   public:
    WaypointDeleteCommand(App& app) : Command(Type::Unknown), app_(app) {};
    virtual ~WaypointDeleteCommand() = default;

    virtual std::string name() const { return "Waypoint Delete"; }
    virtual void execute() {};
    virtual void revert() {};
};

class WaypointModificationCommand : public Command {
   private:
    App& app_;

   public:
    WaypointModificationCommand(App& app) : Command(Type::Unknown), app_(app) {};
    virtual ~WaypointModificationCommand() = default;

    virtual std::string name() const { return "Waypoint Modification"; }
    virtual void execute() {};
    virtual void revert() {};
};
}  // namespace Midori
