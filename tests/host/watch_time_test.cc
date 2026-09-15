#include "watch_time_tools.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
void TestStopwatch() {
    WatchTimeTools tools;
    tools.StartStopwatch(1000);
    tools.StartStopwatch(1500);
    Check(tools.StopwatchElapsedMs(2000) == 1000, "duplicate start preserves origin");
    Check(tools.AddLap(2000), "running lap captured");
    tools.PauseStopwatch(3000);
    Check(tools.StopwatchElapsedMs(9000) == 2000, "pause freezes elapsed");
    Check(!tools.AddLap(9000), "paused lap rejected");
    tools.StartStopwatch(9000);
    Check(tools.StopwatchElapsedMs(10000) == 3000, "resume excludes paused interval");
    for (size_t i = 1; i < WatchTimeTools::kMaximumLaps; ++i) { Check(tools.AddLap(10000 + i), "bounded lap"); }
    Check(!tools.AddLap(11000), "lap memory bounded");
    tools.ResetStopwatch(12000);
    Check(!tools.StopwatchRunning() && tools.StopwatchElapsedMs(20000) == 0 && tools.Laps().empty(), "reset clears stopwatch");
}
void TestTimer() {
    WatchTimeTools tools;
    Check(!tools.StartTimer(0, 0), "zero timer rejected");
    Check(!tools.StartTimer(WatchTimeTools::kMaximumTimerMs + 1, 0), "excess timer rejected");
    Check(tools.StartTimer(10000, 500), "timer starts");
    tools.PauseTimer(4500);
    Check(tools.TimerRemainingMs(99000) == 6000, "paused remaining time retained");
    Check(tools.ResumeTimer(100000), "timer resumes");
    Check(!tools.Poll(105999, {}).timer_expired, "timer not early");
    Check(tools.Poll(106000, {}).timer_expired, "timer expires without synchronized time");
    Check(!tools.Poll(106001, {}).timer_expired && tools.TimerAlert(), "single expiry event with retained alert");
    tools.DismissTimerAlert();
    Check(!tools.TimerAlert() && !tools.ResumeTimer(107000), "completed timer cannot resume");
    tools.CancelTimer();
    Check(!tools.TimerConfigured(), "timer canceled");
}
void TestAlarms() {
    WatchTimeTools tools;
    Check(tools.SetAlarm(0, {7, 30, 0, true, -1}), "one-shot alarm configured");
    Check(tools.SetAlarm(1, {7, 30, 0x3e, true, -1}), "weekday alarm configured");
    Check(!tools.SetAlarm(5, {}), "alarm count bounded");
    Check(!tools.SetAlarm(2, {24, 0, 0, true, -1}), "invalid hour rejected");
    Check(tools.Poll(0, {false, 100, 1, 7, 30}).alarms.empty(), "unsynchronized alarms suppressed");
    Check(tools.Poll(0, {true, 100, 1, 7, 30}).alarms.size() == 2, "matching alarms delivered");
    Check(tools.Poll(0, {true, 100, 1, 7, 30}).alarms.empty(), "same-minute alarm deduplicated");
    Check(tools.Poll(0, {true, 99, 1, 7, 30}).alarms.empty(), "clock rollback cannot repeat alarm");
    Check(tools.Poll(0, {true, 101, 2, 7, 30}).alarms.size() == 1, "one-shot disabled, weekday repeats");
    Check(tools.Poll(0, {true, 105, 6, 7, 30}).alarms.empty(), "weekend excluded");
}
void TestDeepSleepRestore() {
    WatchTimeTools tools;
    tools.StartStopwatch(1000);
    tools.StartTimer(60000, 1000);
    const auto checkpoint = tools.Checkpoint(11000, 7);
    WatchTimeTools restored;
    Check(restored.Restore(checkpoint, 21000, 7), "valid checkpoint restored");
    Check(restored.StopwatchElapsedMs(21000) == 20000, "sleep included in stopwatch");
    Check(restored.TimerRemainingMs(21000) == 40000, "sleep deducted from countdown");
    Check(!restored.InterruptedByReset(), "same epoch is not interrupted");
    Check(restored.Restore(checkpoint, 71000, 7), "expired timer restored");
    Check(restored.Poll(71000, {}).timer_expired, "overdue timer reported once after wake");
}
void TestColdBootAndInvalidCheckpoint() {
    WatchTimeTools tools;
    tools.StartStopwatch(1000);
    tools.StartTimer(60000, 1000);
    auto checkpoint = tools.Checkpoint(11000, 7);
    WatchTimeTools restored;
    Check(restored.Restore(checkpoint, 200, 8), "cold boot recovers checkpoint");
    Check(restored.InterruptedByReset(), "cold boot reported as interrupted");
    Check(!restored.StopwatchRunning() && restored.StopwatchElapsedMs(50000) == 10000, "unknown off time not invented");
    Check(!restored.TimerRunning() && restored.TimerRemainingMs(50000) == 50000, "cold boot timer paused");
    checkpoint.version = 99;
    Check(!restored.Restore(checkpoint, 20000, 7), "unknown schema rejected");
    checkpoint.version = 1;
    checkpoint.alarms[0].weekdays = 255;
    Check(!restored.Restore(checkpoint, 20000, 7), "invalid alarm rejected atomically");
    Check(restored.InterruptedByReset(), "invalid restore leaves current state intact");
}
void TestOverflowAndBackwardClock() {
    WatchTimeTools tools;
    const auto max = std::numeric_limits<uint64_t>::max();
    tools.StartTimer(100, max - 50);
    Check(tools.TimerRemainingMs(max) == 50, "countdown does not overflow deadline");
    tools.StartStopwatch(500);
    Check(tools.StopwatchElapsedMs(100) == 0, "backward clock does not underflow");
    auto checkpoint = tools.Checkpoint(600, 1);
    checkpoint.stopwatch_elapsed_ms = max - 10;
    Check(tools.Restore(checkpoint, 700, 1), "large elapsed restore");
    Check(tools.StopwatchElapsedMs(900) == max, "elapsed addition saturates");
}
}  // namespace

int main() {
    TestStopwatch(); TestTimer(); TestAlarms(); TestDeepSleepRestore();
    TestColdBootAndInvalidCheckpoint(); TestOverflowAndBackwardClock();
    std::cout << "6 watch time tests passed\n";
}
