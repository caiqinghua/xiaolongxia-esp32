#include "watch/watch_push_to_talk.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>

int main() {
    using Result = WatchPressCompletion;
    WatchPushToTalk intent;
    assert(!intent.Press(0, 0));
    auto token = WatchPushToTalk::NextToken();
    assert(intent.Press(token, 100));
    assert(!intent.Press(token, 101));
    assert(!intent.AllowsInput(token, 101));
    intent.Release(token);
    assert(!intent.CaptureReady(token, 102));
    assert(intent.Finish(token, 102) == Result::kCancel);
    assert(intent.Finish(token, 103) == Result::kNone);
    std::puts("PASS: release during connection prevents capture");

    token = WatchPushToTalk::NextToken();
    assert(intent.Press(token, 200));
    assert(intent.CaptureReady(token, 300));
    assert(intent.AllowsInput(token, 301));
    intent.Release(token);
    assert(!intent.AllowsInput(token, 302));
    assert(intent.Finish(token, 302) == Result::kSend);
    assert(intent.Finish(token, 303) == Result::kNone);
    std::puts("PASS: normal release sends once and closes input immediately");

    for (bool captured : {false, true}) {
        token = WatchPushToTalk::NextToken();
        assert(intent.Press(token, 1000));
        if (captured) assert(intent.CaptureReady(token, 1001));
        intent.Release(token, true);
        assert(!intent.AllowsInput(token, 1002));
        assert(intent.Finish(token, 1002) == Result::kCancel);
    }
    std::puts("PASS: lost gestures cancel before and after capture");

    const auto old_token = token;
    token = WatchPushToTalk::NextToken();
    assert(intent.Press(token, 2000));
    intent.Release(old_token, true);
    intent.Retire(old_token);
    assert(intent.Finish(old_token, 2001) == Result::kNone);
    assert(intent.CaptureReady(token, 2001));
    assert(intent.AllowsInput(token, 2002));
    intent.CancelPending();
    assert(!intent.AllowsInput(token, 2003));
    assert(intent.Finish(token, 2003) == Result::kCancel);
    std::puts("PASS: obsolete cleanup cannot cancel a new press");

    token = WatchPushToTalk::NextToken();
    assert(intent.Press(token, 0));
    assert(intent.Held(token, WatchPushToTalk::kConnectionLimitMs - 1));
    assert(!intent.CaptureReady(token, WatchPushToTalk::kConnectionLimitMs));
    assert(intent.Finish(token, WatchPushToTalk::kConnectionLimitMs) == Result::kTimeout);
    token = WatchPushToTalk::NextToken();
    assert(intent.Press(token, 0));
    assert(intent.CaptureReady(token, 500));
    assert(intent.CaptureReady(token, 1000));
    assert(intent.AllowsInput(token, 500 + WatchPushToTalk::kCaptureLimitMs - 1));
    assert(!intent.AllowsInput(token, 500 + WatchPushToTalk::kCaptureLimitMs));
    assert(intent.Finish(token, 500 + WatchPushToTalk::kCaptureLimitMs) == Result::kSend);
    intent.Release(token);
    assert(intent.Finish(token, 100000) == Result::kNone);
    std::puts("PASS: connection and capture limits are bounded without renewal");

    token = WatchPushToTalk::NextToken();
    assert(intent.Press(token, UINT64_MAX - 100));
    assert(!intent.Held(token, UINT64_MAX - 101));
    assert(intent.CaptureReady(token, UINT64_MAX - 99));
    assert(!intent.AllowsInput(token, UINT64_MAX));
    intent.Retire(token);
    assert(intent.Finish(token, UINT64_MAX) == Result::kNone);
    std::puts("PASS: clock bounds fail closed and retirement is terminal");

    for (int i = 0; i < 100; ++i) {
        token = WatchPushToTalk::NextToken();
        assert(intent.Press(token, 0));
        std::atomic<bool> ready{false};
        std::atomic<bool> released{false};
        std::thread capture([&] {
            ready.store(true);
            intent.CaptureReady(token, 1);
            while (!released.load()) std::this_thread::yield();
            assert(!intent.AllowsInput(token, 2));
            assert(!intent.CaptureReady(token, 2));
        });
        while (!ready.load()) std::this_thread::yield();
        intent.Release(token, true);
        released.store(true);
        capture.join();
        assert(intent.Finish(token, 3) == Result::kCancel);
    }
    std::puts("PASS: concurrent release fences capture independent of owner scheduling");
}
