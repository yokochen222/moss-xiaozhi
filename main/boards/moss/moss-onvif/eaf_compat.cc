#include "eaf_iface.h"

#include <cstring>
#include <cstdlib>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "lib/eaf/gfx_eaf_dec.h"

#define TAG "EafCompat"

esp_err_t eaf_init(const uint8_t* data, size_t data_len, eaf_format_handle_t* ret_parser) {
    return eaf_dec_init(data, data_len, reinterpret_cast<eaf_dec_handle_t*>(ret_parser));
}

esp_err_t eaf_deinit(eaf_format_handle_t handle) {
    return eaf_dec_deinit(reinterpret_cast<eaf_dec_handle_t>(handle));
}

int eaf_get_total_frames(eaf_format_handle_t handle) {
    return eaf_dec_get_total_frames(reinterpret_cast<eaf_dec_handle_t>(handle));
}

eaf_format_type_t eaf_get_frame_info(eaf_format_handle_t handle, int frame_index,
                                     eaf_header_t* frame_info) {
    if (frame_info == nullptr) {
        return EAF_FORMAT_INVALID;
    }
    eaf_dec_header_t hdr = {};
    eaf_dec_type_t type =
        eaf_dec_get_frame_info(reinterpret_cast<eaf_dec_handle_t>(handle), frame_index, &hdr);
    std::memset(frame_info, 0, sizeof(*frame_info));
    frame_info->bit_depth = hdr.bit_depth;
    frame_info->width = hdr.width;
    frame_info->height = hdr.height;
    frame_info->blocks = hdr.blocks;
    frame_info->block_height = hdr.block_height;
    frame_info->data_offset = hdr.data_offset;
    frame_info->num_colors = hdr.num_colors;
    std::memcpy(frame_info->format, hdr.format, sizeof(frame_info->format));
    std::memcpy(frame_info->version, hdr.version, sizeof(frame_info->version));
    // Steal dynamic pointers; caller frees via eaf_free_header().
    frame_info->block_len = hdr.block_len;
    frame_info->palette = hdr.palette;
    hdr.block_len = nullptr;
    hdr.palette = nullptr;
    eaf_dec_free_header(&hdr);
    return static_cast<eaf_format_type_t>(type);
}

void eaf_free_header(eaf_header_t* header) {
    if (header == nullptr) {
        return;
    }
    eaf_dec_header_t hdr = {};
    hdr.block_len = header->block_len;
    hdr.palette = header->palette;
    eaf_dec_free_header(&hdr);
    header->block_len = nullptr;
    header->palette = nullptr;
}

static uint8_t* alloc_aligned16(size_t size) {
    uint8_t* p =
        static_cast<uint8_t*>(heap_caps_aligned_alloc(16, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (p == nullptr) {
        p = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (p == nullptr) {
        void* raw = nullptr;
        if (posix_memalign(&raw, 16, size) == 0) {
            p = static_cast<uint8_t*>(raw);
        }
    }
    return p;
}

static void free_aligned16(void* p) {
    if (p == nullptr) {
        return;
    }
    // heap_caps_aligned_alloc / posix_memalign both free with free()/heap_caps_free.
    heap_caps_free(p);
}

// Local decode with 16-byte aligned JPEG block buffer.
// esp_emote_gfx 3.x uses plain malloc() for JPEG output, which fails on ESP32-S3.
esp_err_t eaf_frame_decode(eaf_format_handle_t handle, int frame_index, uint8_t* frame_buffer,
                           size_t frame_buffer_size, bool swap_bytes) {
    if (handle == nullptr || frame_buffer == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    auto* parser = reinterpret_cast<eaf_dec_handle_t>(handle);
    const uint8_t* frame_data = eaf_dec_get_frame_data(parser, frame_index);
    if (frame_data == nullptr) {
        ESP_LOGE(TAG, "Frame %d data unavailable", frame_index);
        return ESP_FAIL;
    }

    eaf_dec_header_t header = {};
    eaf_dec_type_t format = eaf_dec_get_frame_info(parser, frame_index, &header);
    if (format != EAF_DEC_TYPE_VALID) {
        ESP_LOGE(TAG, "Frame %d header parse failed", frame_index);
        return ESP_FAIL;
    }

    const size_t block_height = header.block_height;
    const size_t width = header.width;
    const size_t height = header.height;
    const uint8_t bit_depth = header.bit_depth;

    size_t block_size = width * block_height;
    if (bit_depth == EAF_COLOR_DEPTH_24BIT) {
        block_size *= 2;
    }

    const size_t needed = width * height * sizeof(uint16_t);
    if (frame_buffer_size < needed) {
        ESP_LOGE(TAG, "Frame buffer too small: need %zu got %zu", needed, frame_buffer_size);
        eaf_dec_free_header(&header);
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t* offsets = static_cast<uint32_t*>(malloc(header.blocks * sizeof(uint32_t)));
    if (offsets == nullptr) {
        ESP_LOGE(TAG, "No mem for block offsets");
        eaf_dec_free_header(&header);
        return ESP_ERR_NO_MEM;
    }
    eaf_dec_calculate_offsets(&header, offsets);

    uint8_t* block_buf = alloc_aligned16(block_size);
    if (block_buf == nullptr) {
        ESP_LOGE(TAG, "No mem for aligned block buffer (%zu)", block_size);
        free(offsets);
        eaf_dec_free_header(&header);
        return ESP_ERR_NO_MEM;
    }

    uint32_t palette_cache[256];
    std::memset(palette_cache, 0xFF, sizeof(palette_cache));

    for (int block = 0; block < header.blocks; block++) {
        const uint8_t* block_data = frame_data + offsets[block];
        int block_len = static_cast<int>(header.block_len[block]);
        esp_err_t ret = eaf_dec_decode_block(&header, block_data, block_len, block_buf, swap_bytes);
        if (ret != ESP_OK) {
            ESP_LOGD(TAG, "Block %d decode failed", block);
            continue;
        }

        auto* dst = reinterpret_cast<uint16_t*>(frame_buffer) + (block * block_height * width);

        size_t valid_size;
        if ((block + 1) * block_height > height) {
            valid_size = (height - block * block_height) * width;
            if (bit_depth == EAF_COLOR_DEPTH_24BIT) {
                valid_size *= 2;
            }
        } else {
            valid_size = block_size;
        }

        if (bit_depth == EAF_COLOR_DEPTH_8BIT) {
            for (size_t i = 0; i < valid_size; i++) {
                uint8_t index = block_buf[i];
                uint16_t color;
                if (palette_cache[index] == 0xFFFFFFFFu) {
                    gfx_color_t eaf_color = {};
                    eaf_dec_get_palette_color(&header, index, swap_bytes, &eaf_color);
                    palette_cache[index] = eaf_color.full;
                    color = eaf_color.full;
                } else {
                    color = static_cast<uint16_t>(palette_cache[index]);
                }
                dst[i] = color;
            }
        } else if (bit_depth == EAF_COLOR_DEPTH_24BIT) {
            std::memcpy(dst, block_buf, valid_size);
        } else {
            ESP_LOGW(TAG, "%d-bit depth not supported", bit_depth);
        }
    }

    free_aligned16(block_buf);
    free(offsets);
    eaf_dec_free_header(&header);
    return ESP_OK;
}
