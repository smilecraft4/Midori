#pragma once

#include <string>

namespace Midori {
class Command {
   public:
    enum class Type {
        None,
        PaintStroke,
        EraseStroker,

        Unknown,
    };

   private:
    Type type_ = Type::Unknown;

   public:
    // Command(const Command &) = delete;
    // Command(Command &&) = delete;
    // Command &operator=(const Command &) = delete;
    // Command &operator=(Command &&) = delete;

    explicit Command(Type type) : type_(type) {};
    virtual ~Command() = default;
    Type type() const { return type_; }

    virtual std::string name() const = 0;
    virtual void execute() = 0;
    virtual void revert() = 0;

    // TODO: Should all RAII thingy be logged in debug to check for memory leaks
};
}  // namespace Midori
