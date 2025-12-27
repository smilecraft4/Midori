#include "midori/history.h"

#include <assert.h>

namespace Midori {

HistoryTree::HistoryTree(size_t capacity) : commands_(capacity) {}

void HistoryTree::store(std::unique_ptr<Command> command) {
    if (pos_ < commands_.size()) {
        commands_.truncate(pos_);  // erase commands ahead of pos_
    }
    commands_.emplace_back(std::move(command));
    pos_ = std::min(pos_ + 1, commands_.max_size());
}
void HistoryTree::undo() {
    if (pos_ > 0) {
        commands_[pos_ - 1]->revert();
        pos_--;
    }
}
void HistoryTree::redo() {
    if (pos_ < commands_.size()) {
        commands_[pos_]->execute();
        pos_++;
    }
}

void HistoryTree::clear() {
    commands_.clear();
    pos_ = 0;
}

size_t HistoryTree::size() const { return commands_.size(); }
size_t HistoryTree::max_size() const { return commands_.max_size(); }
size_t HistoryTree::position() const { return pos_; }

const Command *HistoryTree::get(size_t pos) {
    assert(pos < commands_.size() && "Out of bound");
    return commands_[pos].get();
}

}  // namespace Midori
