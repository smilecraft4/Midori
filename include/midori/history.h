#pragma once

#include <memory>

#include "ring_array.h"

namespace Midori {
class HistoryCommand {
   public:
    HistoryCommand() = default;
    virtual ~HistoryCommand() = default;

    virtual void execute() = 0;
    virtual void revert() = 0;
};

class Test : public HistoryCommand {};

class HistoryTree {
   public:
    HistoryTree(const HistoryTree &) = delete;
    HistoryTree(HistoryTree &&) = delete;
    HistoryTree &operator=(const HistoryTree &) = delete;
    HistoryTree &operator=(HistoryTree &&) = delete;

    HistoryTree(size_t capacity) : commands_(capacity) {}

    void store(std::unique_ptr<HistoryCommand> command) {
        commands_.truncate(pos_);
        commands_.emplace_back(std::move(command));
        pos_ = std::min(pos_ + 1, commands_.max_size());
    }
    void undo() {
        if (pos_ > 0) {
            commands_[pos_]->revert();
            pos_--;
        }
    }
    void redo() {
        if (pos_ < commands_.size()) {
            commands_[pos_]->execute();
            pos_++;
        }
    }

    void clear() {
        commands_.clear();
        pos_ = 0;
    }

    size_t position() const { return pos_; }

   private:
    ring_array<std::unique_ptr<HistoryCommand>> commands_;
    std::size_t pos_{};
};

}  // namespace Midori