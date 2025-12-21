#include <gtest/gtest.h>

#include <array>
#include <memory_resource>
#include <print>

#include "midori/ring_array.h"

class InstanceCounter {
   public:
    InstanceCounter() { instanceCount_++; }
    ~InstanceCounter() { instanceCount_--; }
    InstanceCounter(const InstanceCounter& other) : InstanceCounter() {}
    InstanceCounter& operator=(const InstanceCounter& other) = default;

    static int instanceCount_;
};

int InstanceCounter::instanceCount_ = 0;

TEST(MidoriBase, RingArray_Overwritting) {
    Midori::ring_array<int> ringArray(4);
    ringArray.push_back(0);
    ringArray.push_back(1);
    ringArray.push_back(2);
    ringArray.push_back(3);
    ringArray.push_back(4);
    ringArray.push_back(5);

    auto t = ringArray.front();

    for (size_t i = 0; i < ringArray.size(); i++) {
        switch (i) {
            case 0:
                EXPECT_EQ(ringArray[i], 2);
                break;
            case 1:
                EXPECT_EQ(ringArray[i], 3);
                break;
            case 2:
                EXPECT_EQ(ringArray[i], 4);
                break;
            case 3:
                EXPECT_EQ(ringArray[i], 5);
                break;
            default:
                FAIL();
                break;
        }
    }
}

TEST(MidoriBase, RingArray_RAII) {
    Midori::ring_array<InstanceCounter> ringArray(4);
    ringArray.push_back(InstanceCounter());
    EXPECT_EQ(InstanceCounter::instanceCount_, 1);

    ringArray.push_back(InstanceCounter());
    EXPECT_EQ(InstanceCounter::instanceCount_, 2);

    ringArray.push_back(InstanceCounter());
    EXPECT_EQ(InstanceCounter::instanceCount_, 3);

    ringArray.push_back(InstanceCounter());
    EXPECT_EQ(InstanceCounter::instanceCount_, 4);

    ringArray.push_back(InstanceCounter());
    EXPECT_EQ(InstanceCounter::instanceCount_, 4);

    ringArray.push_back(InstanceCounter());
    EXPECT_EQ(InstanceCounter::instanceCount_, 4);
}