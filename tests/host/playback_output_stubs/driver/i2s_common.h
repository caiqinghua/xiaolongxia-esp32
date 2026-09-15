#pragma once
#include <cstddef>
#include <cstdint>
#include <driver/i2s_std.h>
#include <esp_err.h>
struct i2s_chan_info_t { bool is_enabled; uint32_t total_dma_buf_size; };
int i2s_channel_get_info(i2s_chan_handle_t, i2s_chan_info_t*);
int i2s_channel_enable(i2s_chan_handle_t);
int i2s_channel_disable(i2s_chan_handle_t);
int i2s_channel_write(i2s_chan_handle_t, const void*, size_t, size_t*, uint32_t);
