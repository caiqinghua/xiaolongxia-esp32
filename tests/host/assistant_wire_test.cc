#include "protocols/assistant_wire_format.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

static Json Parse(const std::string& input) {
    return Json(assistant_wire::ParseJson(input.data(), input.size()), cJSON_Delete);
}

static void TestAudioRoundTrip() {
    const uint8_t payload[] = {0x00, 0x42, 0xff};
    for (int version : {1, 2, 3}) {
        std::string wire;
        assert(assistant_wire::EncodeAudio(version, payload, sizeof(payload), 0x87654321, wire));
        const std::string original = wire;
        assistant_wire::AudioView audio;
        assert(assistant_wire::DecodeAudio(version, wire.data(), wire.size(), audio));
        assert(audio.size == sizeof(payload));
        assert(std::memcmp(audio.data, payload, sizeof(payload)) == 0);
        assert(audio.timestamp == (version == 2 ? 0x87654321U : 0));
        assert(wire == original);
        // Unaligned input must be safe on the MCU as well as the host.
        std::string unaligned = "x" + wire;
        assert(assistant_wire::DecodeAudio(version, unaligned.data() + 1, wire.size(), audio));
        if (version == 2) {
            const uint8_t expected[] = {0, 2, 0, 0, 0, 0, 0, 0, 0x87, 0x65, 0x43, 0x21, 0, 0, 0, 3};
            assert(std::memcmp(wire.data(), expected, sizeof(expected)) == 0);
        }
    }
}

static void TestAudioBounds() {
    assistant_wire::AudioView audio;
    for (int version : {2, 3}) {
        std::string wire;
        const uint8_t payload[] = {1, 2, 3};
        assert(assistant_wire::EncodeAudio(version, payload, sizeof(payload), 42, wire));
        for (size_t size = 0; size < wire.size(); ++size) {
            assert(!assistant_wire::DecodeAudio(version, wire.data(), size, audio));
            assert(audio.data == nullptr);
        }
        assert(!assistant_wire::DecodeAudio(version, (wire + "x").data(), wire.size() + 1, audio));
        auto malformed = wire;
        malformed[version == 2 ? 3 : 0] = 1;
        assert(!assistant_wire::DecodeAudio(version, malformed.data(), malformed.size(), audio));
        malformed = wire;
        malformed[version == 2 ? 12 : 2] = static_cast<char>(0xff);
        assert(!assistant_wire::DecodeAudio(version, malformed.data(), malformed.size(), audio));
        if (version == 2) {
            malformed = wire;
            malformed[1] = 3;
            assert(!assistant_wire::DecodeAudio(version, malformed.data(), malformed.size(), audio));
        }
    }
    std::vector<uint8_t> large(assistant_wire::kMaxAudioBytes + 1, 1);
    for (int version : {1, 2, 3}) {
        std::string output = "unchanged";
        assert(!assistant_wire::EncodeAudio(version, large.data(), large.size(), 0, output));
        assert(output == "unchanged");
        assert(assistant_wire::EncodeAudio(version, large.data(), large.size() - 1, 0, output));
        assert(assistant_wire::DecodeAudio(version, output.data(), output.size(), audio));
        assert(!assistant_wire::DecodeAudio(version, nullptr, output.size(), audio));
        assert(!assistant_wire::EncodeAudio(version, nullptr, 1, 0, output));
        assert(!assistant_wire::EncodeAudio(version, large.data(), 0, 0, output));
    }
    assert(!assistant_wire::DecodeAudio(1, large.data(), large.size(), audio));
    assert(!assistant_wire::DecodeAudio(4, large.data(), 1, audio));
}

static void TestJsonBounds() {
    const char raw[] = {'{', '"', 't', 'y', 'p', 'e', '"', ':', '"', 'x', '"', '}'};
    Json json(assistant_wire::ParseJson(raw, sizeof(raw)), cJSON_Delete);
    assert(json);
    assert(Parse("{\"type\":\"tts\"}\n \t\r"));
    for (const char* invalid : {"", "{}", "[]", "null", "{\"type\":1}", "{\"type\":\"\"}",
                                "{\"type\":\"x\"}garbage", "{\"type\":\"x\"}{}", "{\"type\":\"x\""}) {
        assert(!Parse(invalid));
    }
    std::string nul = "{\"type\":\"x\"}";
    nul.push_back('\0');
    assert(!Parse(nul));
    assert(!Parse(std::string(assistant_wire::kMaxJsonBytes + 1, ' ')));
    assert(!Parse("{\"type\":\"x\",\"data\":" + std::string(33, '[') + "0" + std::string(33, ']') + "}"));
    assert(Parse(R"({"type":"x","text":"[{\"quoted\"}]"})"));
    assert(!assistant_wire::ParseJson(nullptr, 1));
}

static void TestHelloCompatibility() {
    assistant_wire::ServerHello hello;
    for (int version : {1, 2, 3}) {
        auto root = Parse(R"({"type":"hello","transport":"websocket"})");
        assert(assistant_wire::ParseHello(root.get(), version, hello));
        assert(hello.sample_rate == 24000 && hello.frame_duration == 60 && hello.session_id.empty());
        root = Parse(R"({"type":"hello","transport":"websocket","session_id":"test-session","audio_params":{"sample_rate":16000,"frame_duration":20,"format":"opus","channels":1}})");
        assert(assistant_wire::ParseHello(root.get(), version, hello));
        assert(hello.sample_rate == 16000 && hello.frame_duration == 20 && hello.session_id == "test-session");
    }
}

static void TestHelloValidation() {
    for (const char* invalid : {
             R"({"type":"hello"})", R"({"type":"hello","transport":1})",
             R"({"type":"hello","transport":null})", R"({"type":"hello","transport":"mqtt"})",
             R"({"type":"hello","transport":"websocket","session_id":1})",
             R"({"type":"hello","transport":"websocket","version":3})",
             R"({"type":"hello","transport":"websocket","audio_params":null})",
             R"({"type":"hello","transport":"websocket","audio_params":{"sample_rate":0}})",
             R"({"type":"hello","transport":"websocket","audio_params":{"sample_rate":16000.5}})",
             R"({"type":"hello","transport":"websocket","audio_params":{"sample_rate":"16000"}})",
             R"({"type":"hello","transport":"websocket","audio_params":{"frame_duration":2.5}})",
             R"({"type":"hello","transport":"websocket","audio_params":{"frame_duration":1000000000}})",
             R"({"type":"hello","transport":"websocket","audio_params":{"channels":2}})",
             R"({"type":"hello","transport":"websocket","audio_params":{"format":"pcm"}})"}) {
        auto root = Parse(invalid);
        assert(root);
        assistant_wire::ServerHello hello{8000, 10, "unchanged"};
        assert(!assistant_wire::ParseHello(root.get(), 2, hello));
        assert(hello.sample_rate == 8000 && hello.frame_duration == 10 && hello.session_id == "unchanged");
    }
    auto root = Parse("{\"type\":\"hello\",\"transport\":\"websocket\",\"session_id\":\"" + std::string(257, 'x') + "\"}");
    assistant_wire::ServerHello hello;
    assert(!assistant_wire::ParseHello(root.get(), 1, hello));
    assert(!assistant_wire::ParseHello(nullptr, 1, hello));
}

int main() {
    TestAudioRoundTrip();
    TestAudioBounds();
    TestJsonBounds();
    TestHelloCompatibility();
    TestHelloValidation();
    std::cout << "PASS: 5 assistant wire-format test groups\n";
}
