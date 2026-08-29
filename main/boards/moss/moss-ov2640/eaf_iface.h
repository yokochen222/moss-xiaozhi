#pragma once

// Compatibility wrappers over espressif2022/esp_emote_gfx eaf_dec_* (v3.x).
// Splash code keeps the older eaf_* names used by bread-compact-wifi-096lcd.

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EAF_MAGIC_HEAD 0x5A5A
#define EAF_MAGIC_LEN 2

typedef void* eaf_format_handle_t;

typedef enum {
    EAF_FORMAT_VALID = 0,
    EAF_FORMAT_REDIRECT = 1,
    EAF_FORMAT_INVALID = 2,
    EAF_FORMAT_FLAG = 3
} eaf_format_type_t;

typedef struct {
    char format[3];
    char version[6];
    uint8_t bit_depth;
    uint16_t width;
    uint16_t height;
    uint16_t blocks;
    uint16_t block_height;
    uint32_t* block_len;
    uint16_t data_offset;
    uint8_t* palette;
    int num_colors;
} eaf_header_t;

esp_err_t eaf_init(const uint8_t* data, size_t data_len, eaf_format_handle_t* ret_parser);
esp_err_t eaf_deinit(eaf_format_handle_t handle);
int eaf_get_total_frames(eaf_format_handle_t handle);
eaf_format_type_t eaf_get_frame_info(eaf_format_handle_t handle, int frame_index,
                                     eaf_header_t* frame_info);
void eaf_free_header(eaf_header_t* header);
esp_err_t eaf_frame_decode(eaf_format_handle_t handle, int frame_index, uint8_t* frame_buffer,
                           size_t frame_buffer_size, bool swap_bytes);

#ifdef __cplusplus
}
#endif
