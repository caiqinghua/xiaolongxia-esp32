#include "watch/watch_wifi_gate.h"
#include <cassert>
#include <cstdio>
#include <future>
#include <thread>

int main() {
    WatchWifiGate gate;
    int starts = 0;
    auto start = [&]() { ++starts; return true; };
    assert(gate.Run(start));
    gate.Initialize(false);
    assert(!gate.Run(start) && starts == 1);
    gate.Observe(1, true);
    assert(!gate.Enabled() && !gate.Run(start));
    gate.Observe(2, false);
    assert(!gate.ConfirmEnabled(1) && !gate.ConfirmEnabled(2));
    assert(!gate.IsCurrent(1) && gate.IsCurrent(2));
    gate.Observe(3, true);
    assert(gate.ConfirmEnabled(3) && gate.Run(start) && starts == 2);
    gate.Observe(0, false);
    assert(gate.Enabled() && gate.IsCurrent(3));
    std::puts("PASS: WiFi starts require confirmed current enable intent");

    assert(!gate.Run({}));
    assert(gate.Run([&]() { return gate.Run(start); }));
    gate.Observe(4, false);
    bool stopped = false;
    assert(gate.Run([&]() { stopped = true; return true; }, false));
    assert(stopped && !gate.Run(start));
    std::puts("PASS: disabled policy still allows serialized stop and nested driver callbacks");

    gate.Observe(5, true);
    assert(gate.ConfirmEnabled(5));
    std::promise<void> entered, release;
    auto release_future = release.get_future();
    std::thread radio([&]() {
        assert(gate.Run([&]() {
            entered.set_value();
            release_future.wait();
            return true;
        }));
    });
    entered.get_future().wait();
    // UI intent must not wait on the potentially blocking driver operation.
    gate.Observe(6, false);
    assert(!gate.Enabled());
    std::atomic<bool> stop_finished{false};
    std::promise<void> stop_called;
    std::thread stop([&]() {
        stop_called.set_value();
        assert(gate.Run([&]() { stop_finished = true; return true; }, false));
    });
    stop_called.get_future().wait();
    assert(!stop_finished.load());
    release.set_value();
    radio.join();
    stop.join();
    assert(stop_finished.load() && !gate.Run(start));
    std::puts("PASS: disable is immediate while stop waits for in-flight radio startup");

    gate.Observe(7, true);
    std::promise<void> canceled;
    std::thread ui([&]() { gate.Observe(8, false); canceled.set_value(); });
    canceled.get_future().wait();
    assert(!gate.ConfirmEnabled(7) && !gate.Enabled());
    ui.join();
    std::puts("PASS: superseded enable cannot restore permission after cancellation");

    gate.Observe(9, true);
    assert(gate.ConfirmEnabled(9));
    assert(gate.BeginTransfer([]() { return true; }));
    assert(gate.TransferActive());
    assert(!gate.Run(start) && !gate.Run(start, false));
    assert(!gate.BeginTransfer([]() { return true; }));
    gate.Observe(10, false);
    assert(!gate.Run(start, false));
    assert(!gate.EndTransfer([]() { return false; }));
    assert(gate.TransferActive());
    assert(gate.EndTransfer([]() { return true; }));
    assert(!gate.TransferActive() && !gate.Run(start));
    assert(gate.Run([]() { return true; }, false));
    std::puts("PASS: transfer owns WiFi through disabled policy and failed cleanup until checked release");

    gate.Observe(11, true);
    assert(gate.ConfirmEnabled(11));
    assert(!gate.BeginTransfer([]() { return false; }));
    assert(gate.TransferActive() && !gate.Run(start));
    assert(gate.EndTransfer([]() { return true; }));
    assert(gate.Run(start));
    assert(!gate.EndTransfer([]() { return true; }));
    std::puts("PASS: partially failed AP startup cannot expose the radio to station startup before cleanup");
    gate.SetStationAllowed(false);
    assert(gate.Enabled() && !gate.RunStation(start));
    assert(gate.BeginTransfer([]() { return true; }));
    assert(!gate.RunStation(start));
    assert(gate.EndTransfer([]() { return true; }));
    assert(!gate.RunStation(start));
    gate.SetStationAllowed(true);
    assert(gate.RunStation(start));
    gate.Observe(12, false);
    assert(!gate.StationAllowed() && !gate.RunStation(start));
    gate.SetStationAllowed(true);
    assert(!gate.RunStation(start));
    std::puts("PASS: suspended station preserves AP permission without overriding an explicit Wi-Fi disable");
}
