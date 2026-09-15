#include "playback_codec_adapter.h"
#include <opus.h>
#include "esp_opus_dec.h"

namespace HostPlaybackCodec {
std::atomic<Fault> fault{Fault::kNone};
std::atomic<bool> fail_close{false};
std::atomic<int> handles{0};
std::atomic<int> decodes{0};
}
namespace {
struct Decoder { OpusDecoder* opus; uint32_t rate; };
}

// Test boundary only: actual ESP declarations, reference libopus on the host.
extern "C" esp_audio_err_t esp_opus_dec_reset(void* handle) {
    if (!handle || HostPlaybackCodec::fault == HostPlaybackCodec::Fault::kReset) return ESP_AUDIO_ERR_FAIL;
    return opus_decoder_ctl(static_cast<Decoder*>(handle)->opus, OPUS_RESET_STATE) == OPUS_OK ? ESP_AUDIO_ERR_OK : ESP_AUDIO_ERR_FAIL;
}

extern "C" esp_audio_err_t esp_opus_dec_open(void* config, uint32_t size, void** handle) {
    *handle = nullptr;
    if (size != sizeof(esp_opus_dec_cfg_t) || HostPlaybackCodec::fault == HostPlaybackCodec::Fault::kOpen)
        return ESP_AUDIO_ERR_FAIL;
    auto* cfg = static_cast<esp_opus_dec_cfg_t*>(config);
    if (cfg->channel != 1 || cfg->self_delimited || cfg->frame_duration != ESP_OPUS_DEC_FRAME_DURATION_120_MS)
        return ESP_AUDIO_ERR_INVALID_PARAMETER;
    int error = 0;
    auto* opus = opus_decoder_create(static_cast<opus_int32>(cfg->sample_rate), 1, &error);
    if (!opus || error != OPUS_OK) return ESP_AUDIO_ERR_FAIL;
    *handle = new Decoder{opus, cfg->sample_rate};
    ++HostPlaybackCodec::handles;
    return ESP_AUDIO_ERR_OK;
}

extern "C" esp_audio_err_t esp_opus_dec_close(void* handle) {
    if (HostPlaybackCodec::fail_close) return ESP_AUDIO_ERR_FAIL;
    if (!handle) return ESP_AUDIO_ERR_INVALID_PARAMETER;
    auto* decoder = static_cast<Decoder*>(handle);
    opus_decoder_destroy(decoder->opus);
    delete decoder;
    --HostPlaybackCodec::handles;
    return ESP_AUDIO_ERR_OK;
}

extern "C" esp_audio_err_t esp_opus_dec_decode(void* handle, esp_audio_dec_in_raw_t* raw,
    esp_audio_dec_out_frame_t* frame, esp_audio_dec_info_t* info) {
    const auto fault = HostPlaybackCodec::fault.load();
    ++HostPlaybackCodec::decodes;
    if (fault == HostPlaybackCodec::Fault::kDecode || raw->frame_recover != ESP_AUDIO_DEC_RECOVERY_NONE)
        return ESP_AUDIO_ERR_FAIL;
    auto* decoder = static_cast<Decoder*>(handle);
    const int samples = opus_decode(decoder->opus, raw->buffer, static_cast<opus_int32>(raw->len),
        reinterpret_cast<opus_int16*>(frame->buffer), static_cast<int>(frame->len / sizeof(int16_t)), 0);
    if (samples <= 0) return ESP_AUDIO_ERR_FAIL;
    raw->consumed = fault == HostPlaybackCodec::Fault::kConsumption ? raw->len - 1 : raw->len;
    frame->decoded_size = fault == HostPlaybackCodec::Fault::kSize ? frame->len + 2 : samples * sizeof(int16_t);
    *info = {};
    info->sample_rate = fault == HostPlaybackCodec::Fault::kFormat ? 0 : decoder->rate;
    info->channel = 1;
    info->bits_per_sample = 16;
    return ESP_AUDIO_ERR_OK;
}
