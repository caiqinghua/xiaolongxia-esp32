#include "watch/watch_speech_interrupt.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <thread>

using Detector = WatchSpeechInterrupt;

static bool Frame(Detector& detector, uint64_t generation, uint64_t time,
                  bool speech = true, int16_t value = 1) {
    std::array<int16_t, 320> pcm;
    pcm.fill(value);
    return detector.Feed(generation, pcm.data(), pcm.size(), speech, time);
}

static uint64_t Qualify(Detector& detector, uint64_t generation, uint64_t start = 300) {
    for (uint64_t i = 0; i < 11; ++i) assert(!Frame(detector, generation, start + i * 20));
    assert(Frame(detector, generation, start + 220));
    return start + 220;
}

int main() {
    {
        Detector detector;
        std::vector<int16_t> prefix{99};
        assert(!detector.Begin(0, 0));
        assert(detector.Begin(7, 0) && !detector.Begin(8, 0));
        assert(!Frame(detector, 8, 10000));
        for (uint64_t t = 0; t < 300; t += 20) assert(!Frame(detector, 7, t));
        Qualify(detector, 7);
        assert(!Frame(detector, 7, 540, false, 2));
        assert(!detector.Take(8, 540, prefix) && prefix.empty());
        assert(detector.Take(7, 540, prefix));
        assert(prefix.size() == 13 * 320);
        assert(std::count(prefix.begin(), prefix.end(), 1) == 12 * 320);
        assert(std::count(prefix.begin(), prefix.end(), 2) == 320);
        assert(!detector.Take(7, 540, prefix) && prefix.empty());
        assert(!Frame(detector, 7, 560));
        assert(detector.Begin(8, 560));
        detector.Cancel();
        assert(!Frame(detector, 8, 900));
    }
    {
        Detector detector;
        assert(detector.Begin(1, 0));
        for (uint64_t i = 0; i < 100; ++i)
            assert(!Frame(detector, 1, 300 + i * 20, i % 3 != 0));
        // Short bursts separated by silence cannot accumulate into an onset.
        auto end = Qualify(detector, 1, 2500);
        std::vector<int16_t> prefix;
        assert(detector.Take(1, end, prefix));
        assert(prefix.size() <= Detector::kPrerollSamples);
    }
    {
        Detector detector;
        assert(detector.Begin(1, 0));
        for (uint64_t i = 0; i < 10; ++i) assert(!Frame(detector, 1, 300 + i * 20));
        // A long callback gap discards pre-gap speech and audio.
        Qualify(detector, 1, 1000);
        std::vector<int16_t> prefix;
        assert(detector.Take(1, 1220, prefix) && prefix.size() == 3840);
    }
    {
        Detector detector;
        assert(detector.Begin(1, 0));
        for (int i = 0; i < 1000; ++i) assert(!Frame(detector, 1, 300));
        detector.Cancel();
        assert(detector.Begin(2, 0));
        // A single late small frame is not 240 ms of speech.
        assert(!Frame(detector, 2, 300));
        assert(!Frame(detector, 2, 500));
        assert(!Frame(detector, 2, 700));
    }
    {
        Detector detector;
        std::vector<int16_t> prefix;
        assert(detector.Begin(1, 0));
        for (uint64_t i = 0; i < 200; ++i) assert(!Frame(detector, 1, 300 + i * 20, false, i));
        Qualify(detector, 1, 4300);
        assert(detector.Take(1, 4520, prefix));
        assert(prefix.size() == Detector::kPrerollSamples);
        // Ring wrap retains the last eight silent frames, then all onset frames.
        for (size_t i = 0; i < 8; ++i)
            assert(std::all_of(prefix.begin() + i * 320, prefix.begin() + (i + 1) * 320,
                              [i](int16_t x) { return x == 192 + static_cast<int>(i); }));
        assert(std::all_of(prefix.begin() + 8 * 320, prefix.end(), [](int16_t x) { return x == 1; }));
    }
    for (int failure = 0; failure < 6; ++failure) {
        Detector detector;
        assert(detector.Begin(1, 0));
        Qualify(detector, 1);
        std::vector<int16_t> prefix;
        if (failure == 0) assert(!detector.Take(1, 721, prefix));
        if (failure == 1) assert(!detector.Take(1, 519, prefix));
        if (failure == 2) assert(!detector.Feed(1, nullptr, 320, true, 540));
        std::array<int16_t, Detector::kMaximumFrameSamples + 1> invalid{};
        if (failure == 3) assert(!detector.Feed(1, invalid.data(), invalid.size(), true, 540));
        if (failure == 4) assert(!detector.Feed(1, invalid.data(), 0, true, 540));
        if (failure == 5) {
            for (uint64_t t = 540; t <= 1040; t += 20) assert(!Frame(detector, 1, t));
        }
        assert(!detector.Take(1, 1040, prefix));
        assert(detector.Begin(2, 1040));
    }
    {
        Detector detector;
        assert(detector.Begin(1, 0));
        Qualify(detector, 1);
        // Deliberately oversized burst after qualification fails closed instead
        // of wrapping over the beginning of the user's utterance.
        std::array<int16_t, Detector::kMaximumFrameSamples> pcm{};
        for (int i = 0; i < 10; ++i)
            assert(!detector.Feed(1, pcm.data(), pcm.size(), true, 540));
        std::vector<int16_t> prefix;
        assert(!detector.Take(1, 540, prefix));
    }
    {
        Detector detector;
        assert(detector.Begin(1, 0));
        std::thread producer([&] {
            for (uint64_t i = 0; i < 1000; ++i) Frame(detector, 1, 300 + i * 20);
        });
        detector.Cancel();
        assert(detector.Begin(2, 0));
        producer.join();
        Qualify(detector, 2);
        std::vector<int16_t> prefix;
        assert(detector.Take(2, 520, prefix) && prefix.size() == 3840);
    }
    std::cout << "PASS: speech interrupt qualification, bounded preroll and generation isolation\n";
}
