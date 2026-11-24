#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace di::integrations {

struct HistrioProfile {
    std::string patternId;
    std::string patternVersion;
    std::optional<std::string> tone;
};

struct HistrioScriptPattern {
    std::string id;
    std::string version;
    std::string label;
    std::string healthy;
    std::string stress;
};

template <class T>
struct HistrioResult {
    std::optional<T> value;
    std::string error;
};

class HistrioClient {
public:
    static constexpr std::chrono::milliseconds kDeadline{1500};

    explicit HistrioClient(std::string base_url);
    bool enabled() const { return !base_url_.empty(); }
    HistrioResult<HistrioProfile> profile(std::string_view persona_id) const;
    HistrioResult<std::vector<HistrioScriptPattern>> script_patterns() const;

private:
    std::string base_url_;
};

} // namespace di::integrations
