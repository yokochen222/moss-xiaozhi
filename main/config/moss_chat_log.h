#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct MossChatLine {
    uint32_t seq = 0;
    std::string role;
    std::string text;
    std::string state;
};

class MossChatLog {
public:
    static MossChatLog& GetInstance();

    void OnRelay(const std::string& event, const std::string& role, const std::string& text,
                 const std::string& state);
    uint32_t Seq() const;
    std::string Voice() const;
    std::vector<MossChatLine> Since(uint32_t seq) const;

private:
    static constexpr size_t kCap = 32;
    static constexpr size_t kTextMax = 512;

    mutable std::mutex mutex_;
    uint32_t seq_ = 0;
    std::string voice_ = "idle";
    MossChatLine lines_[kCap]{};
    size_t head_ = 0;
    size_t count_ = 0;
};
