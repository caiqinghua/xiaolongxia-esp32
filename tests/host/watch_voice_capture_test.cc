#include "watch/watch_voice_capture.h"
#include <cassert>
#include <iostream>
#include <thread>

using Result = WatchVoiceCaptureResult;

int main() {
    WatchVoiceCapture capture;
    assert(!capture.Begin(0, 0));
    assert(capture.Begin(1, 0));
    assert(!capture.Begin(2, 0));
    assert(capture.AllowsInput(1, 7999));
    capture.Vad(1, true, 8000);
    assert(!capture.AllowsInput(1, 8000));
    assert(capture.Poll(1, 8000) == Result::kCancel);
    assert(capture.Poll(1, 8001) == Result::kIdle);

    assert(capture.Begin(2, 10000));
    capture.Vad(1, true, 10001);
    assert(capture.Poll(1, 20000) == Result::kIdle);
    capture.Vad(2, true, 10100);
    capture.Vad(2, true, 10200); // Repeated VAD must not reset speech duration.
    capture.Vad(2, false, 10300);
    assert(capture.AllowsInput(2, 11199));
    assert(!capture.AllowsInput(2, 11200));
    capture.Vad(2, true, 11200); // Delayed main-loop polling cannot reopen a finished utterance.
    assert(capture.Poll(2, 11200) == Result::kSend);
    assert(!capture.AllowsInput(2, 11201));
    assert(capture.Poll(2, 11201) == Result::kIdle);

    assert(capture.Begin(3, 20000));
    capture.Vad(3, true, 20100);
    capture.Vad(3, false, 20120); // Short impulse is not an utterance.
    assert(capture.Poll(3, 22000) == Result::kListening);
    assert(capture.Poll(3, 28000) == Result::kCancel);

    assert(capture.Begin(4, 30000));
    capture.Vad(4, true, 30100);
    capture.Vad(4, false, 30400);
    capture.Vad(4, true, 30800); // Natural short pauses stay in the same capture.
    assert(capture.Poll(4, 32000) == Result::kListening);
    assert(!capture.AllowsInput(4, 90000));
    assert(capture.Poll(4, 90000) == Result::kSend);

    assert(capture.Begin(5, 100000));
    capture.Vad(5, true, 100100);
    capture.Cancel();
    assert(capture.Begin(6, 100200));
    std::thread stale([&]() { for (int i = 0; i < 100; ++i) capture.Vad(5, true, 100300 + i); });
    stale.join();
    assert(capture.Poll(6, 108200) == Result::kCancel);
    assert(capture.Begin(7, 200000));
    assert(capture.Poll(7, 199999) == Result::kCancel);
    assert(capture.Begin(8, 300000));
    assert(!capture.SeedSpeech(7, 240, 300000));
    assert(!capture.SeedSpeech(8, 149, 300000));
    assert(!capture.SeedSpeech(8, 1001, 300000));
    assert(!capture.SeedSpeech(8, 240, 300001));
    assert(capture.SeedSpeech(8, 240, 300000));
    assert(!capture.SeedSpeech(8, 240, 300000));
    assert(capture.Poll(8, 300899) == Result::kListening);
    assert(capture.Poll(8, 300900) == Result::kSend);
    assert(capture.Begin(9, 400000));
    assert(capture.SeedSpeech(9, 240, 400000));
    capture.Vad(9, true, 400100);
    capture.Vad(9, false, 400700);
    assert(capture.Poll(9, 401599) == Result::kListening);
    assert(capture.Poll(9, 401600) == Result::kSend);
    std::cout << "PASS local wake capture: onset, minimum speech, silence, hard limit, single completion and stale VAD\n";
}
