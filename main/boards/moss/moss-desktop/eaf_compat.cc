#include "eaf_iface.h"

#include <cstring>

#include "lib/eaf/gfx_eaf_dec.h"

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

esp_err_t eaf_frame_decode(eaf_format_handle_t handle, int frame_index, uint8_t* frame_buffer,
                           size_t frame_buffer_size, bool swap_bytes) {
    return eaf_dec_decode_frame(reinterpret_cast<eaf_dec_handle_t>(handle), frame_index,
                                frame_buffer, frame_buffer_size, swap_bytes);
}
