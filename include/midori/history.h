#pragma once

#include <memory>

#include "command.h"
#include "ring_array.h"

namespace Midori {

class HistoryTree {
   public:
    HistoryTree(const HistoryTree &) = delete;
    HistoryTree(HistoryTree &&) = delete;
    HistoryTree &operator=(const HistoryTree &) = delete;
    HistoryTree &operator=(HistoryTree &&) = delete;

    HistoryTree(size_t capacity) : commands_(capacity) {}

    void store(std::unique_ptr<Command> command) {
        if (pos_ < commands_.size()) {
            commands_.truncate(pos_);  // erase commands ahead of pos_
        }
        commands_.emplace_back(std::move(command));
        pos_ = std::min(pos_ + 1, commands_.max_size());
    }
    void undo() {
        if (pos_ > 0) {
            commands_[pos_ - 1]->revert();
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

    size_t size() { return commands_.size(); }
    size_t max_size() { return commands_.max_size(); }
    size_t position() const { return pos_; }

    const Command *get(size_t pos) {
        assert(pos < commands_.size() && "Out of bound");
        return commands_[pos].get();
    }

   private:
    ring_array<std::unique_ptr<Command>> commands_;
    std::size_t pos_{};
};

}  // namespace Midori