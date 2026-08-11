#include "moss_splash.h"
#include <cstring>
#include <cstdio>
#include <vector>

#include "esp_log.h"

namespace moss_splash {

#pragma pack(push, 1)
struct MmapHeader {
    char     magic[4];        // "MMAP"
    uint32_t version;
    uint32_t name_len;
    uint32_t files;
    uint32_t checksum;
    uint32_t payload_len;
    uint32_t reserved[2];
};
#pragma pack(pop)

bool find_asset_in_bin(const uint8_t* base, size_t size, const char* name, AssetInfo& out) {
    if (base == nullptr || name == nullptr) {
        return false;
    }
    if (size < 32) {
        return false;
    }
    if (std::memcmp(base, "MMAP", 4) != 0) {
        return false;
    }

    MmapHeader hdr;
    std::memcpy(&hdr, base, sizeof(MmapHeader));

    const uint32_t name_len = hdr.name_len;
    const uint32_t stride   = name_len + 12;
    const uint32_t table_len = hdr.files * stride;
    const uint32_t table_start = sizeof(MmapHeader);
    const uint32_t data_start  = table_start + table_len;

    if (data_start > size) {
        return false;
    }

    for (uint32_t i = 0; i < hdr.files; i++) {
        const uint8_t* entry = base + table_start + i * stride;
        const char* asset_name = (const char*)entry;
        if (std::strcmp(asset_name, name) == 0) {
            uint32_t payload_size = *(const uint32_t*)(entry + name_len);
            uint32_t offset       = *(const uint32_t*)(entry + name_len + 4);
            uint16_t w            = *(const uint16_t*)(entry + name_len + 8);
            uint16_t h            = *(const uint16_t*)(entry + name_len + 10);
            out.name = asset_name;
            // 跳过 0x5A5A ASSETS_FILE_MAGIC_HEAD (eaf_init 期望数据从 0x89EAF 开始)
            out.data = base + data_start + offset + 2;
            out.size = payload_size;
            out.width = w;
            out.height = h;
            return true;
        }
    }
    return false;
}

bool find_asset(const char* name, AssetInfo& out) {
    return find_asset_in_bin(emote_assets_bin, emote_assets_bin_size(), name, out);
}

int get_animation_fps(const uint8_t* bin_base, size_t bin_size, const char* asset_name) {
    if (bin_base == nullptr || asset_name == nullptr) return 20;
    if (bin_size < 32) return 20;

    // MMAP header: 32 bytes
    MmapHeader hdr;
    std::memcpy(&hdr, bin_base, sizeof(MmapHeader));
    if (std::memcmp(hdr.magic, "MMAP", 4) != 0) return 20;

    const uint32_t name_len   = hdr.name_len;
    const uint32_t num_files  = hdr.files;
    const uint32_t stride     = name_len + 12;
    const uint32_t data_start = sizeof(MmapHeader) + num_files * stride;
    if (data_start > bin_size) return 20;

    // 找 index.json (entry 名以 "index.json" 开头)
    for (uint32_t i = 0; i < num_files; i++) {
        const uint8_t* entry = bin_base + sizeof(MmapHeader) + i * stride;
        if (std::strncmp((const char*)entry, "index.json", 10) != 0) continue;

        uint32_t payload_size = *(const uint32_t*)(entry + name_len);
        uint32_t offset       = *(const uint32_t*)(entry + name_len + 4);
        uint32_t json_start   = data_start + offset + 2;  // skip 0x5A5A header
        if (json_start + payload_size > bin_size) return 20;

        const char* json = (const char*)(bin_base + json_start);
        const char* json_end = json + payload_size;

        // 找 "asset_name" 块, 然后其后面的 "fps": N
        // 简单状态机: 找 '"asset_name"' 然后向后找 '"fps"' 后面跟数字
        const size_t name_len_str = std::strlen(asset_name);
        for (const char* p = json; p + name_len_str < json_end; p++) {
            if (p[0] != '"') continue;
            if (std::strncmp(p + 1, asset_name, name_len_str) != 0) continue;
            if (p[1 + name_len_str] != '"') continue;
            // 找到了 "asset_name", 现在向后找 "fps"
            for (const char* q = p + 2 + name_len_str; q + 5 < json_end; q++) {
                if (q[0] != '"') continue;
                if (std::strncmp(q, "\"fps\"", 5) != 0) continue;
                // 找到 "fps", 读数字
                const char* r = q + 5;
                while (r < json_end && (*r == ' ' || *r == ':' || *r == '\t' || *r == '\n')) r++;
                int fps = 0;
                while (r < json_end && *r >= '0' && *r <= '9') {
                    fps = fps * 10 + (*r - '0');
                    r++;
                }
                if (fps > 0 && fps <= 120) return fps;
                break;  // 只匹配最近的一个 fps
            }
            break;  // 只匹配 asset_name 第一次出现
        }
        break;  // 找到 index.json 后停止
    }
    return 20;
}

}  // namespace moss_splash