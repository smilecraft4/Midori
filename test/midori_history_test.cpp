#include <gtest/gtest.h>

#include "midori/history.h"

static size_t command_count = 0;

class PaintStroke : public Midori::Command {
   public:
    PaintStroke() : Command(Command::Type::Unknown) {
        // GTEST_LOG_(INFO) << "PaintStroke()";
        command_count++;
    };
    virtual ~PaintStroke() {
        // GTEST_LOG_(INFO) << "~PaintStroke()";
        command_count--;
    }
    PaintStroke(const PaintStroke& other) : PaintStroke() {
        // GTEST_LOG_(INFO) << "PaintStroke(const PaintStroke& other) : PaintStroke()";
    }
    PaintStroke& operator=(const PaintStroke& other) = default;

    virtual std::string name() const { return "PaintStroke"; };

    virtual void execute() {
        // GTEST_LOG_(INFO) << "PaintStroke::execute()";
    }
    virtual void revert() {
        // GTEST_LOG_(INFO) << "PaintStroke::revert()";
    }
};

class EraseStroke : public Midori::Command {
   public:
    EraseStroke() : Command(Command::Type::Unknown) {
        // GTEST_LOG_(INFO) << "EraseStroke()";
        command_count++;
    };
    virtual ~EraseStroke() {
        // GTEST_LOG_(INFO) << "~EraseStroke()";
        command_count--;
    }
    EraseStroke(const EraseStroke& other) : EraseStroke() {
        // GTEST_LOG_(INFO) << "EraseStroke(const EraseStroke& other) : EraseStroke()";
    }
    EraseStroke& operator=(const EraseStroke& other) = default;

    virtual std::string name() const { return "EraseStroke"; };

    virtual void execute() {
        // GTEST_LOG_(INFO) << "EraseStroke::execute()";
    }
    virtual void revert() {
        // GTEST_LOG_(INFO) << "EraseStroke::revert()";
    }
};

class EditLayer : public Midori::Command {
   public:
    EditLayer() : Command(Command::Type::Unknown) {
        // GTEST_LOG_(INFO) << "EditLayer()";
        command_count++;
    };
    virtual ~EditLayer() {
        // GTEST_LOG_(INFO) << "~EditLayer()";
        command_count--;
    }
    EditLayer(const EditLayer& other) : EditLayer() {
        //  GTEST_LOG_(INFO) << "EditLayer(const EditLayer& other) : EditLayer()";
    }
    EditLayer& operator=(const EditLayer& other) = default;

    virtual std::string name() const { return "EditLayer"; };

    virtual void execute() {
        // GTEST_LOG_(INFO) << "EditLayer::execute()";
    }
    virtual void revert() {
        // GTEST_LOG_(INFO) << "EditLayer::revert()";
    }
};

class DeleteLayer : public Midori::Command {
   public:
    DeleteLayer() : Command(Command::Type::Unknown) {
        // GTEST_LOG_(INFO) << "DeleteLayer()";
        command_count++;
    };
    virtual ~DeleteLayer() {
        // GTEST_LOG_(INFO) << "~DeleteLayer()";
        command_count--;
    }
    DeleteLayer(const DeleteLayer& other) : DeleteLayer() {
        // GTEST_LOG_(INFO) << "DeleteLayer(const DeleteLayer& other) : DeleteLayer()";
    }
    DeleteLayer& operator=(const DeleteLayer& other) = default;

    virtual std::string name() const { return "DeleteLayer"; };

    virtual void execute() {
        // GTEST_LOG_(INFO) << "DeleteLayer::execute()";
    }
    virtual void revert() {
        // GTEST_LOG_(INFO) << "DeleteLayer::revert()";
    }
};

class CreateLayer : public Midori::Command {
   public:
    CreateLayer() : Command(Command::Type::Unknown) {
        //  GTEST_LOG_(INFO) << "CreateLayer()";
        command_count++;
    };
    virtual ~CreateLayer() {
        //  GTEST_LOG_(INFO) << "~CreateLayer()";
        command_count--;
    }
    CreateLayer(const CreateLayer& other) : CreateLayer() {
        // GTEST_LOG_(INFO) << "CreateLayer(const CreateLayer& other) : CreateLayer()";
    }
    CreateLayer& operator=(const CreateLayer& other) = default;

    virtual std::string name() const { return "CreateLayer"; };

    virtual void execute() {  // GTEST_LOG_(INFO) << "CreateLayer::execute()";
    }
    virtual void revert() {  // GTEST_LOG_(INFO) << "CreateLayer::revert()";
    }
};

TEST(MidoriHistory, Adding) {
    {
        Midori::HistoryTree tree(4);

        tree.store(std::make_unique<PaintStroke>());
        EXPECT_EQ(command_count, 1);
        tree.store(std::make_unique<PaintStroke>());
        EXPECT_EQ(command_count, 2);
        tree.store(std::make_unique<EditLayer>());
        EXPECT_EQ(command_count, 3);
        tree.store(std::make_unique<DeleteLayer>());
        EXPECT_EQ(command_count, 4);
        tree.store(std::make_unique<CreateLayer>());
        EXPECT_EQ(command_count, 4);
        tree.store(std::make_unique<EraseStroke>());
        EXPECT_EQ(command_count, 4);
    }
    EXPECT_EQ(command_count, 0);
}

TEST(MidoriHistory, Undoing) {
    {
        Midori::HistoryTree tree(4);
        EXPECT_EQ(tree.position(), 0);

        tree.store(std::make_unique<PaintStroke>());
        EXPECT_EQ(tree.position(), 1);
        tree.store(std::make_unique<PaintStroke>());
        EXPECT_EQ(tree.position(), 2);
        tree.store(std::make_unique<EditLayer>());
        EXPECT_EQ(tree.position(), 3);
        tree.store(std::make_unique<DeleteLayer>());
        EXPECT_EQ(tree.position(), 4);
        tree.store(std::make_unique<EditLayer>());
        EXPECT_EQ(tree.position(), 4);
        tree.store(std::make_unique<PaintStroke>());
        EXPECT_EQ(tree.position(), 4);

        tree.undo();
        EXPECT_EQ(tree.position(), 3);
        tree.undo();
        EXPECT_EQ(tree.position(), 2);
        tree.undo();
        EXPECT_EQ(tree.position(), 1);
        tree.undo();
        EXPECT_EQ(tree.position(), 0);
        tree.undo();
        EXPECT_EQ(tree.position(), 0);
    }
}

TEST(MidoriHistory, Redoing) {
    {
        Midori::HistoryTree tree(4);
        EXPECT_EQ(tree.position(), 0);

        tree.store(std::make_unique<PaintStroke>());
        EXPECT_EQ(tree.position(), 1);
        tree.store(std::make_unique<PaintStroke>());
        EXPECT_EQ(tree.position(), 2);
        tree.store(std::make_unique<EditLayer>());
        EXPECT_EQ(tree.position(), 3);
        tree.store(std::make_unique<DeleteLayer>());
        EXPECT_EQ(tree.position(), 4);
        tree.store(std::make_unique<EditLayer>());
        EXPECT_EQ(tree.position(), 4);
        tree.store(std::make_unique<PaintStroke>());
        EXPECT_EQ(tree.position(), 4);

        tree.redo();
        EXPECT_EQ(tree.position(), 4);

        tree.undo();
        EXPECT_EQ(tree.position(), 3);
        tree.undo();
        EXPECT_EQ(tree.position(), 2);
        tree.undo();
        EXPECT_EQ(tree.position(), 1);
        tree.undo();
        EXPECT_EQ(tree.position(), 0);
        tree.redo();
        EXPECT_EQ(tree.position(), 1);
        tree.redo();
        EXPECT_EQ(tree.position(), 2);
        tree.redo();
        EXPECT_EQ(tree.position(), 3);
        tree.redo();
        EXPECT_EQ(tree.position(), 4);
        tree.redo();
        EXPECT_EQ(tree.position(), 4);
    }
}

TEST(MidoriHistory, StoringFreeing) {
    Midori::HistoryTree tree(4);
    EXPECT_EQ(tree.position(), 0);

    tree.store(std::make_unique<PaintStroke>());
    EXPECT_EQ(tree.position(), 1);
    tree.store(std::make_unique<PaintStroke>());
    EXPECT_EQ(tree.position(), 2);
    tree.store(std::make_unique<EditLayer>());
    EXPECT_EQ(tree.position(), 3);
    tree.store(std::make_unique<DeleteLayer>());
    EXPECT_EQ(tree.position(), 4);

    tree.undo();
    EXPECT_EQ(tree.position(), 3);
    tree.undo();
    EXPECT_EQ(tree.position(), 2);
    EXPECT_EQ(command_count, 4);

    tree.store(std::make_unique<PaintStroke>());
    EXPECT_EQ(command_count, 3);
    tree.store(std::make_unique<PaintStroke>());
    EXPECT_EQ(command_count, 4);
}