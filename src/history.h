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

    HistoryTree(size_t capacity);

    void store(std::unique_ptr<Command> command);
    void undo();
    void redo();
    void clear();

    size_t size() const;
    size_t max_size() const;
    size_t position() const;

    const Command *get(size_t pos);

   private:
    ring_array<std::unique_ptr<Command>> commands_;
    std::size_t pos_{};
};

}  // namespace Midori