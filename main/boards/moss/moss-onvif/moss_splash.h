#pragma once

#include <cstdint>
#include <cstddef>

// MOSS 开机画面 - 嵌入的 MMAP 格式 EAF 动画
// 由 ESP Emote GFX Packer NEXT 打包, 嵌入到固件 (CMakeLists.txt EMBED_FILES)
//
// MMAP 文件格式 (32字节 header + asset table + payloads):
//   0x00: "MMAP" magic (4)
//   0x04: version (4)
//   0x08: name_len (4)
//   0x0C: files (4)
//   0x10: checksum (4)
//   0x14: payload_len (4)
//   0x18: reserved (8)
//   0x20: asset table, 每个 entry = name(name_len) + size(4) + offset(4) + width(2) + height(2)
//   ...: payload 块, 每个 payload 起始有 0x5A5A (ASSETS_FILE_MAGIC_HEAD), 实际文件数据从 +2 开始

namespace moss_splash {

// 嵌入的 emote-assets.bin 数据 (由 ESP-IDF 的 EMBED_FILES 在链接时提供符号)
// ESP-IDF 生成以下符号:
//   emote_assets_bin, _binary_emote_assets_bin_end, emote_assets_bin_length
extern "C" {
    extern const uint8_t emote_assets_bin[];
    extern const uint32_t emote_assets_bin_length;
}

// 计算嵌入文件大小
static inline size_t emote_assets_bin_size() {
    return (size_t)emote_assets_bin_length;
}

// 解析后的 asset 信息
struct AssetInfo {
    const char* name;
    const uint8_t* data;  // 去掉 ASSETS_FILE_MAGIC_HEAD (0x5A5A) 后的数据
    uint32_t size;        // 完整 payload size (含 0x5A5A 头)
    uint16_t width;
    uint16_t height;
};

// 在嵌入的 emote-assets.bin 中按文件名查找 asset
// 返回 true 表示找到
bool find_asset(const char* name, AssetInfo& out);

// 在指定的 MMAP bin (base, size) 中按文件名查找 asset
bool find_asset_in_bin(const uint8_t* base, size_t size, const char* name, AssetInfo& out);

// 从 MMAP bin 的 index.json 中解析指定动画的 FPS
// 返回 fps (1..120), 默认 20
int get_animation_fps(const uint8_t* bin_base, size_t bin_size, const char* asset_name);

}  // namespace moss_splash
