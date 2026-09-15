#include "watch_stream_encoder.h"
#include "esp_opus_enc.h"

bool WatchStreamEncoder::Encode(void* encoder, WatchEncodeRole role, uint64_t stream,
    const std::vector<int16_t>& pcm, size_t frame_samples, size_t output_bytes,
    std::vector<uint8_t>& packet) {
    packet.clear();
    if (!encoder || !stream) return false;
    const bool changed = encoder_ != encoder || role_ != role || stream_ != stream;
    if (changed) {
        encoder_ = encoder; role_ = role; stream_ = stream;
        failed_ = esp_opus_enc_reset(encoder) != ESP_AUDIO_ERR_OK;
    }
    if (failed_) return false;
    // Current watch capture is mono 16 kHz, at most a 120 ms Opus packet.
    if (!frame_samples || frame_samples > 1920 || pcm.size() != frame_samples ||
        !output_bytes || output_bytes > 8192) { failed_ = true; return false; }
    packet.resize(output_bytes);
    esp_audio_enc_in_frame_t input{};
    input.buffer = reinterpret_cast<uint8_t*>(const_cast<int16_t*>(pcm.data()));
    input.len = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    esp_audio_enc_out_frame_t output{};
    output.buffer = packet.data();
    output.len = static_cast<uint32_t>(packet.size());
    const auto result = esp_opus_enc_process(encoder, &input, &output);
    if (result != ESP_AUDIO_ERR_OK || !output.encoded_bytes || output.encoded_bytes > packet.size()) {
        failed_ = true; packet.clear(); return false;
    }
    packet.resize(output.encoded_bytes);
    return true;
}
