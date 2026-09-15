#include "esp_tcp.h"
#include "esp_ssl.h"
#include "web_socket.h"
#include "network_interface.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <vector>

using namespace std::chrono_literals;
static std::mutex io_mutex;
static std::condition_variable io_cv;
static bool peer_closed = false;
static bool shutdown_requested = false;
static std::atomic<int> readers{0};
static std::atomic<int> closes{0};
static std::atomic<int> shutdowns{0};
static std::atomic<int> groups{0};
static std::atomic<int> waits{0};
static bool create_task_ok = true;
static bool create_group_ok = true;
static std::vector<std::unique_ptr<HostTransportTask>> tasks;
static thread_local TaskHandle_t current_task = nullptr;

EventGroupHandle_t xEventGroupCreate() {
    if (!create_group_ok) return nullptr;
    ++groups;
    return new HostTransportEvents;
}
void vEventGroupDelete(EventGroupHandle_t group) { --groups; delete group; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) {
    std::lock_guard<std::mutex> lock(group->mutex);
    group->bits |= bits;
    group->cv.notify_all();
    return group->bits;
}
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) {
    std::lock_guard<std::mutex> lock(group->mutex);
    auto old = group->bits;
    group->bits &= ~bits;
    return old;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits, bool clear, bool, TickType_t timeout) {
    ++waits;
    std::unique_lock<std::mutex> lock(group->mutex);
    // Bound broken tests without changing the firmware's requested wait value.
    const auto duration = timeout == portMAX_DELAY ? 3s : std::chrono::milliseconds(timeout);
    group->cv.wait_for(lock, duration, [&] { return (group->bits & bits) != 0; });
    if (timeout == portMAX_DELAY) assert((group->bits & bits) != 0);
    const auto result = group->bits;
    if (clear) group->bits &= ~bits;
    return result;
}
BaseType_t xTaskCreate(void (*entry)(void*), const char*, unsigned, void* arg, unsigned, TaskHandle_t* handle) {
    if (!create_task_ok) return 0;
    auto task = std::make_unique<HostTransportTask>();
    *handle = task.get();
    task->thread = std::thread([entry, arg, self = task.get()] { current_task = self; entry(arg); });
    tasks.push_back(std::move(task));
    return pdPASS;
}
TaskHandle_t xTaskGetCurrentTaskHandle() { return current_task; }
void vTaskDelete(TaskHandle_t) {}
void vTaskDelay(TickType_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

extern "C" int host_socket(int, int, int) { return 7; }
extern "C" int host_connect(int, const sockaddr*, socklen_t) { return 0; }
extern "C" int host_close(int) {
    assert(readers == 0);
    ++closes;
    return 0;
}
extern "C" int host_shutdown(int, int) {
    ++shutdowns;
    std::lock_guard<std::mutex> lock(io_mutex);
    shutdown_requested = true;
    io_cv.notify_all();
    return 0;
}
extern "C" ssize_t host_recv(int, void*, size_t, int) {
    std::unique_lock<std::mutex> lock(io_mutex);
    ++readers;
    io_cv.notify_all();
    io_cv.wait(lock, [] { return peer_closed || shutdown_requested; });
    --readers;
    return 0;
}
extern "C" ssize_t host_send(int, const void*, size_t size, int) { return static_cast<ssize_t>(size); }
extern "C" hostent* host_gethostbyname(const char*) {
    static in_addr address{};
    static char* addresses[] = {reinterpret_cast<char*>(&address), nullptr};
    static hostent host{nullptr, nullptr, AF_INET, sizeof(address), addresses};
    return &host;
}

esp_tls_t* esp_tls_init() { return new esp_tls_t; }
int esp_tls_conn_new_sync(const char*, size_t, int, const esp_tls_cfg_t* cfg, esp_tls_t*) {
    assert(cfg->timeout_ms == 10000 && cfg->crt_bundle_attach != nullptr);
    return 1;
}
esp_err_t esp_tls_get_error_handle(esp_tls_t*, esp_tls_error_handle_t*) { return -1; }
esp_err_t esp_tls_get_and_clear_last_error(esp_tls_error_handle_t, int*, int*) { return -1; }
esp_err_t esp_tls_get_conn_sockfd(esp_tls_t* tls, int* fd) { *fd = tls->fd; return ESP_OK; }
int esp_tls_conn_destroy(esp_tls_t* tls) { host_close(tls->fd); delete tls; return 0; }
int esp_tls_conn_read(esp_tls_t* tls, void* data, size_t size) { return static_cast<int>(host_recv(tls->fd, data, size, 0)); }
int esp_tls_conn_write(esp_tls_t* tls, const void* data, size_t size) { return static_cast<int>(host_send(tls->fd, data, size, 0)); }

static void Reset() {
    for (auto& task : tasks) if (task->thread.joinable()) task->thread.join();
    tasks.clear();
    assert(groups == 0 && readers == 0);
    peer_closed = shutdown_requested = false;
    closes = shutdowns = waits = 0;
    create_task_ok = true;
    create_group_ok = true;
}

template<class Client> static void TestActiveShutdown() {
    Reset();
    {
        Client client;
        assert(client.Connect("host.test", 443));
        {
            std::unique_lock<std::mutex> lock(io_mutex);
            assert(io_cv.wait_for(lock, 2s, [] { return readers.load() == 1; }));
        }
        client.Disconnect();
        assert(!client.connected() && readers == 0 && closes == 1 && shutdowns == 1);
        client.Disconnect();
        assert(closes == 1);
    }
    Reset();
}

template<class Client> static void TestPassiveShutdownWaitsForCallback() {
    Reset();
    std::promise<void> callback_entered;
    std::promise<void> callback_release;
    auto release = callback_release.get_future().share();
    auto client = std::make_unique<Client>();
    client->OnDisconnected([&] { callback_entered.set_value(); release.wait(); });
    assert(client->Connect("host.test", 443));
    {
        std::lock_guard<std::mutex> lock(io_mutex);
        peer_closed = true;
        io_cv.notify_all();
    }
    assert(callback_entered.get_future().wait_for(2s) == std::future_status::ready);
    assert(!client->connected());
    auto destroy = std::async(std::launch::async, [&] { client.reset(); });
    assert(destroy.wait_for(30ms) == std::future_status::timeout);
    assert(closes == 0 && groups == 1);
    callback_release.set_value();
    assert(destroy.wait_for(2s) == std::future_status::ready);
    destroy.get();
    assert(closes == 1 && groups == 0);
    Reset();
}

template<class Client> static void TestTaskCreationFailure() {
    Reset();
    create_task_ok = false;
    {
        Client client;
        assert(!client.Connect("host.test", 443));
        assert(client.GetLastError() != 0);
        assert(!client.connected() && closes == 1 && waits == 0);
    }
    assert(closes == 1 && waits == 0);
    Reset();
}

template<class Client> static void TestCallbackShutdownIsJoinedByOwner() {
    Reset();
    Client client;
    std::promise<void> callback_done;
    client.OnDisconnected([&] {
        client.Disconnect();
        assert(closes == 0);
        callback_done.set_value();
    });
    assert(client.Connect("host.test", 443));
    {
        std::lock_guard<std::mutex> lock(io_mutex);
        peer_closed = true;
        io_cv.notify_all();
    }
    assert(callback_done.get_future().wait_for(2s) == std::future_status::ready);
    client.Disconnect();
    assert(closes == 1 && readers == 0);
}

template<class Client> static void TestEventGroupAllocationFailure() {
    Reset();
    create_group_ok = false;
    {
        Client client;
        assert(!client.Connect("host.test", 443));
        assert(client.GetLastError() != 0);
        assert(closes == 0 && waits == 0 && groups == 0);
    }
    Reset();
}

template<class Client> static void TestCloseDeadlineRetainsResources() {
    Reset();
    std::promise<void> callback_entered;
    std::promise<void> callback_release;
    auto release = callback_release.get_future().share();
    {
        Client client;
        client.OnDisconnected([&] { callback_entered.set_value(); release.wait(); });
        assert(client.Connect("host.test", 443));
        {
            std::lock_guard<std::mutex> lock(io_mutex);
            peer_closed = true;
            io_cv.notify_all();
        }
        assert(callback_entered.get_future().wait_for(2s) == std::future_status::ready);
        const auto start = std::chrono::steady_clock::now();
        assert(!client.DisconnectWithin(10));
        assert(std::chrono::steady_clock::now() - start < 500ms);
        assert(closes == 0 && groups == 1);
        assert(!client.DisconnectWithin(0));
        assert(closes == 0 && groups == 1);
        callback_release.set_value();
        assert(client.DisconnectWithin(2000));
        assert(closes == 1 && groups == 1);
        assert(client.DisconnectWithin(0));
        assert(closes == 1);
    }
    Reset();
}

class ImmediateTcp : public Tcp {
public:
    inline static bool close_ready = true;
    inline static int live = 0;
    std::string response = "HTTP/1.1 101 Switching Protocols\r\n\r\n";
    std::vector<std::string> sent;
    ImmediateTcp() { ++live; }
    ~ImmediateTcp() override { Disconnect(); --live; }
    bool Connect(const std::string&, int) override {
        assert(stream_callback_ && disconnect_callback_);
        connected_ = true;
        return true;
    }
    void Disconnect() override {
        if (connected_.exchange(false)) {
            assert(groups > 0);
            disconnect_callback_();
        }
    }
    int Send(const std::string& data) override {
        sent.push_back(data);
        if (data.rfind("GET ", 0) == 0) Emit(response);
        return static_cast<int>(data.size());
    }
    void Emit(const std::string& data) { stream_callback_(data); }
    int GetLastError() override { return 0; }
    bool DisconnectWithin(uint32_t) override {
        if (!close_ready) return false;
        Disconnect();
        return true;
    }
};
class ImmediateNetwork : public NetworkInterface {
public:
    ImmediateTcp* last = nullptr;
    std::string response = "HTTP/1.1 101 Switching Protocols\r\n\r\n";
    std::unique_ptr<Tcp> CreateTcp(int) override {
        auto tcp = std::make_unique<ImmediateTcp>();
        tcp->response = response;
        last = tcp.get();
        return tcp;
    }
    std::unique_ptr<Tcp> CreateSsl(int) override { return CreateTcp(0); }
};

static void TestImmediateWebsocketHandshake() {
    Reset();
    ImmediateNetwork network;
    int closed = 0;
    {
        WebSocket socket(&network, 1);
        socket.OnDisconnected([&] { ++closed; });
        assert(socket.Connect("ws://host.test/audio"));
        assert(socket.IsConnected());
    }
    assert(closed == 1 && groups == 0);
}

static void TestWebsocketEventGroupAllocationFailure() {
    Reset();
    create_group_ok = false;
    ImmediateNetwork network;
    {
        WebSocket socket(&network, 1);
        assert(!socket.Connect("ws://host.test/audio"));
        assert(!socket.IsConnected());
    }
    Reset();
}

static void TestWebsocketCloseDeadline() {
    Reset();
    ImmediateNetwork network;
    {
        WebSocket socket(&network, 1);
        assert(socket.Connect("ws://host.test/audio"));
        ImmediateTcp::close_ready = false;
        assert(!socket.DisconnectWithin(0));
        assert(!socket.IsConnected() && ImmediateTcp::live == 1 && groups == 1);
        ImmediateTcp::close_ready = true;
        assert(socket.DisconnectWithin(0));
        assert(ImmediateTcp::live == 0 && groups == 1);
        assert(socket.DisconnectWithin(0));
    }
    Reset();
}

static std::string ServerFrame(uint8_t opcode, const std::string& payload, bool fin = true) {
    std::string result(1, static_cast<char>((fin ? 0x80 : 0) | opcode));
    const uint64_t length = payload.size();
    if (length < 126) {
        result.push_back(static_cast<char>(length));
    } else if (length < 65536) {
        result.push_back(126);
        result.push_back(static_cast<char>(length >> 8));
        result.push_back(static_cast<char>(length));
    } else {
        result.push_back(127);
        for (int shift = 56; shift >= 0; shift -= 8) result.push_back(static_cast<char>(length >> shift));
    }
    result += payload;
    return result;
}

static void TestWebsocketFragmentationAndCoalescing() {
    Reset();
    ImmediateNetwork network;
    std::vector<std::pair<std::string, bool>> received;
    {
        WebSocket socket(&network, 1);
        socket.OnData([&](const char* data, size_t size, bool binary) {
            received.emplace_back(std::string(data, size), binary);
        });
        assert(socket.Connect("ws://host.test/audio"));
        const auto fragments = ServerFrame(2, "first", false) + ServerFrame(9, "ping") +
                               ServerFrame(0, "second", false) + ServerFrame(0, "third");
        for (char byte : fragments) network.last->Emit(std::string(1, byte));
        assert(received.size() == 1 && received[0].first == "firstsecondthird" && received[0].second);
        assert((static_cast<uint8_t>(network.last->sent.back()[0]) & 0x0f) == 10);
        const std::string maximum(65536, 'a');
        network.last->Emit(ServerFrame(1, maximum) + ServerFrame(2, maximum) + ServerFrame(1, "tail"));
        assert(received.size() == 4 && received[1].first == maximum && !received[1].second);
        assert(received[2].first == maximum && received[2].second && received[3].first == "tail");
        network.last->Emit(ServerFrame(1, "") + ServerFrame(2, ""));
        assert(received.size() == 6 && received[4].first.empty() && received[5].first.empty());
    }
    Reset();
}

static void TestWebsocketRejectsInvalidFrames() {
    // Only headers are supplied for oversized lengths: rejection must not wait for payload.
    const std::vector<std::string> invalid = {
        ServerFrame(0, "orphan"), ServerFrame(3, "reserved"),
        std::string("\xC1\x00", 2), std::string("\x81\x80", 2),
        ServerFrame(9, "fragmented", false), ServerFrame(9, std::string(126, 'p')).substr(0, 2),
        ServerFrame(8, "x"), std::string("\x81\x7e\x00\x01", 4),
        std::string("\x82\x7f\xff\xff\xff\xff\xff\xff\xff\xff", 10),
        ServerFrame(1, std::string(65537, 'x')).substr(0, 10),
        ServerFrame(2, "partial", false) + ServerFrame(1, "new"),
        ServerFrame(2, std::string(65530, 'x'), false) + ServerFrame(0, "overflow")
    };
    for (const auto& bytes : invalid) {
        Reset();
        ImmediateNetwork network;
        int received = 0, errors = 0, closed = 0;
        {
            WebSocket socket(&network, 1);
            socket.OnData([&](const char*, size_t, bool) { ++received; });
            socket.OnError([&](int) { ++errors; });
            socket.OnDisconnected([&] { ++closed; });
            assert(socket.Connect("ws://host.test/audio"));
            network.last->Emit(bytes + ServerFrame(1, "must not be delivered"));
            network.last->Emit(ServerFrame(2, "late data"));
            assert(!socket.IsConnected() && errors == 1 && closed == 1 && received == 0);
            assert(socket.DisconnectWithin(0));
            assert(closed == 1);
        }
        Reset();
    }
}

static void TestWebsocketCloseStopsDelivery() {
    Reset();
    ImmediateNetwork network;
    int received = 0, closed = 0;
    {
        WebSocket socket(&network, 1);
        socket.OnData([&](const char*, size_t, bool) { ++received; });
        socket.OnDisconnected([&] { ++closed; });
        assert(socket.Connect("ws://host.test/audio"));
        network.last->Emit(ServerFrame(8, "") + ServerFrame(2, "after close"));
        network.last->Emit(ServerFrame(2, "later"));
        assert(!socket.IsConnected() && closed == 1 && received == 0);
        assert(socket.DisconnectWithin(0));
        assert(closed == 1);
    }
    Reset();
}

static void TestWebsocketHandshakeBoundsAndTail() {
    for (const auto& response : {std::string(8192, 'x'),
             std::string("HTTP/1.1 200 OK\r\nX-Test: HTTP/1.1 101\r\n\r\n"),
             std::string("HTTP/1.1 1010 Invalid\r\n\r\n")}) {
        Reset();
        ImmediateNetwork network;
        network.response = response;
        WebSocket socket(&network, 1);
        assert(!socket.Connect("ws://host.test/audio"));
        assert(!socket.IsConnected());
    }
    Reset();
    ImmediateNetwork network;
    network.response += ServerFrame(2, "hello");
    int received = 0;
    {
        WebSocket socket(&network, 1);
        socket.OnData([&](const char* data, size_t size, bool binary) {
            assert(binary && std::string(data, size) == "hello");
            ++received;
        });
        assert(socket.Connect("ws://host.test/audio"));
        assert(received == 1);
    }
    // A close in the upgrade's TCP chunk must not be resurrected by Connect().
    network.response = "HTTP/1.1 101 Switching Protocols\r\n\r\n" + ServerFrame(8, "");
    {
        WebSocket socket(&network, 1);
        assert(!socket.Connect("ws://host.test/audio"));
        assert(!socket.IsConnected());
    }
    Reset();
}

int main() {
    TestActiveShutdown<EspTcp>();
    TestActiveShutdown<EspSsl>();
    TestPassiveShutdownWaitsForCallback<EspTcp>();
    TestPassiveShutdownWaitsForCallback<EspSsl>();
    TestTaskCreationFailure<EspTcp>();
    TestTaskCreationFailure<EspSsl>();
    TestCallbackShutdownIsJoinedByOwner<EspTcp>();
    TestCallbackShutdownIsJoinedByOwner<EspSsl>();
    TestEventGroupAllocationFailure<EspTcp>();
    TestEventGroupAllocationFailure<EspSsl>();
    TestCloseDeadlineRetainsResources<EspTcp>();
    TestCloseDeadlineRetainsResources<EspSsl>();
    TestImmediateWebsocketHandshake();
    TestWebsocketEventGroupAllocationFailure();
    TestWebsocketCloseDeadline();
    TestWebsocketFragmentationAndCoalescing();
    TestWebsocketRejectsInvalidFrames();
    TestWebsocketCloseStopsDelivery();
    TestWebsocketHandshakeBoundsAndTail();
    std::cout << "PASS: 19 patched transport lifecycle and receive test groups\n";
}
