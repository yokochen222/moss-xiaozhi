#include "moss_chat_log.h"

MossChatLog& MossChatLog::GetInstance() {
    static MossChatLog instance;
    return instance;
}

void MossChatLog::OnRelay(const std::string& event, const std::string& role, const std::string& text,
                          const std::string& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state.empty()) {
        voice_ = state;
    }
    if (event != "message" || text.empty()) {
        return;
    }
    ++seq_;
    MossChatLine line;
    line.seq = seq_;
    line.role = role.empty() ? "user" : role;
    line.text = text.size() > kTextMax ? text.substr(0, kTextMax) : text;
    line.state = voice_;
    if (count_ == kCap) {
        head_ = (head_ + 1) % kCap;
        --count_;
    }
    lines_[(head_ + count_) % kCap] = std::move(line);
    ++count_;
}

uint32_t MossChatLog::Seq() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seq_;
}

std::string MossChatLog::Voice() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return voice_;
}

std::vector<MossChatLine> MossChatLog::Since(uint32_t seq) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MossChatLine> out;
    for (size_t i = 0; i < count_; ++i) {
        const MossChatLine& line = lines_[(head_ + i) % kCap];
        if (line.seq > seq) out.push_back(line);
    }
    return out;
}
