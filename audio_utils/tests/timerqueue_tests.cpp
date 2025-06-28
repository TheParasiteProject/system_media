/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <audio_utils/TimerQueue.h>

#include <audio_utils/mutex.h>  // audio_utils::unique_lock has thread safety annotations
#include <chrono>
#include <condition_variable>
#include <gtest/gtest.h>
#include <utils/SystemClock.h>

using namespace std::chrono_literals;

namespace android::audio_utils {

class TimerQueueTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::lock_guard lock(mMutex);
        mExecuted = false;
    }

    void TearDown() override {
    }

    std::mutex mMutex;
    std::condition_variable mCv;
    bool mExecuted GUARDED_BY(mMutex);
};

TEST_F(TimerQueueTest, AddAndExecute) {
    TimerQueue tq;

    ASSERT_TRUE(tq.ready());

    const auto executionTime = elapsedRealtimeNano() + 20'000'000; // 20 ms

    tq.add([this]() {
        std::lock_guard lock(mMutex);
        mExecuted = true;
        mCv.notify_one();
    }, executionTime);

    audio_utils::unique_lock ul(mMutex);
    const auto timepoint = std::chrono::steady_clock::now() + 100ms;
    do {
        const auto status = mCv.wait_until(ul, timepoint);
        EXPECT_EQ(std::cv_status::no_timeout, status);
    } while (!mExecuted);
}

TEST_F(TimerQueueTest, Remove) {
    TimerQueue tq;

    ASSERT_TRUE(tq.ready());

    const auto executionTime = elapsedRealtimeNano() + 50'000'000; // 50 ms

    auto handle = tq.add([this]() {
        std::lock_guard lock(mMutex);
        mExecuted = true;
        mCv.notify_one();
    }, executionTime);

    ASSERT_NE(handle, TimerQueue::INVALID_HANDLE);

    bool removed = tq.remove(handle);
    ASSERT_TRUE(removed);

    audio_utils::unique_lock ul(mMutex);
    const auto timepoint = std::chrono::steady_clock::now() + 100ms;
    std::cv_status status = std::cv_status::no_timeout;
    do {
        status = mCv.wait_until(ul, timepoint);
        EXPECT_FALSE(mExecuted);
    } while (status == std::cv_status::no_timeout);
    EXPECT_EQ(std::cv_status::timeout, status);
}

TEST_F(TimerQueueTest, MultipleEvents) {
    TimerQueue tq;

    ASSERT_TRUE(tq.ready());

    std::vector<int> executionOrder;
    std::mutex orderMutex;

    const auto t1 = elapsedRealtimeNano() + 40'000'000; // 40ms
    const auto t2 = elapsedRealtimeNano() + 20'000'000; // 20ms
    const auto t3 = elapsedRealtimeNano() + 60'000'000; // 60ms

    tq.add([&]() {
        std::unique_lock<std::mutex> lock(orderMutex);
        executionOrder.push_back(1);
    }, t1);

    tq.add([&]() {
        std::unique_lock<std::mutex> lock(orderMutex);
        executionOrder.push_back(2);
        mCv.notify_one(); // Notify after the first event
    }, t2);

    tq.add([&]() {
        std::unique_lock<std::mutex> lock(orderMutex);
        executionOrder.push_back(3);
    }, t3);

    // Wait for the first event to fire
    {
        std::unique_lock<std::mutex> lock(mMutex);
        mCv.wait_for(lock, 100ms);
    }

    // Wait a bit longer for other events
    std::this_thread::sleep_for(100ms);

    std::unique_lock<std::mutex> lock(orderMutex);
    ASSERT_EQ(3UL, executionOrder.size());
    EXPECT_EQ(2, executionOrder[0]);
    EXPECT_EQ(1, executionOrder[1]);
    EXPECT_EQ(3, executionOrder[2]);
}

TEST_F(TimerQueueTest, Destructor) {
    {
        TimerQueue tq;
        ASSERT_TRUE(tq.ready());

        const auto executionTime = elapsedRealtimeNano() + 50'000'000; // 50 ms
        tq.add([this]() {
            std::lock_guard lock(mMutex);
            mExecuted = true;
        }, executionTime);
    } // tq is destroyed here

    std::this_thread::sleep_for(100ms);
    std::lock_guard lock(mMutex);
    EXPECT_FALSE(mExecuted);
}

TEST_F(TimerQueueTest, RemoveInvalidHandle) {
    TimerQueue tq;
    ASSERT_TRUE(tq.ready());
    EXPECT_FALSE(tq.remove(12345));
    EXPECT_FALSE(tq.remove(TimerQueue::INVALID_HANDLE));
}

TEST_F(TimerQueueTest, AddNullFunction) {
    TimerQueue tq;
    ASSERT_TRUE(tq.ready());
    const auto handle = tq.add(nullptr, elapsedRealtimeNano() + 10'000'000);
    EXPECT_EQ(handle, TimerQueue::INVALID_HANDLE);
}

} // namespace android::audio_utils
