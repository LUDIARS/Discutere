#include "base/fsutil.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

#include <lorica/input_budget.hpp>

#include "base/crypto.hpp"
#include "base/strings.hpp"

namespace di::fs {
namespace stdfs = std::filesystem;

stdfs::path to_path(std::string_view utf8) {
    std::u8string u8(utf8.size(), u8'\0');
    for (std::size_t i = 0; i < utf8.size(); ++i) u8[i] = static_cast<char8_t>(utf8[i]);
    return stdfs::path(u8);
}

std::string from_path(const stdfs::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

std::optional<std::string> read_file(std::string_view path, std::size_t max_bytes) {
    std::error_code ec;
    const auto p = to_path(path);
    const auto size = stdfs::file_size(p, ec);
    if (ec) return std::nullopt;
    lorica::input_budget budget({max_bytes, 1, 1, 1});
    if (!budget.consume(static_cast<std::size_t>(size), 1, 1)) return std::nullopt;
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::string data(static_cast<std::size_t>(size), '\0');
    in.read(data.data(), static_cast<std::streamsize>(size));
    data.resize(static_cast<std::size_t>(in.gcount()));
    // 読んでいる間に伸びたファイルは、上限までで切らずに読み直しもしない (size 時点の内容で返す)。
    return data;
}

bool ensure_dir(std::string_view path) {
    if (path.empty()) return true;
    std::error_code ec;
    stdfs::create_directories(to_path(path), ec);
    return !ec || stdfs::is_directory(to_path(path), ec);
}

bool write_file_atomic(std::string_view path, std::string_view data) {
    const auto p = to_path(path);
    std::error_code ec;
    if (p.has_parent_path()) stdfs::create_directories(p.parent_path(), ec);
    auto tmp = p;
    tmp += to_path(".tmp-" + crypto::hex_encode(crypto::random_bytes(6)));
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) return false;
    }
    stdfs::rename(tmp, p, ec);
    if (ec) {
        stdfs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool append_file(std::string_view path, std::string_view data) {
    const auto p = to_path(path);
    std::error_code ec;
    if (p.has_parent_path()) stdfs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::app);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

bool exists(std::string_view path) {
    std::error_code ec;
    return stdfs::exists(to_path(path), ec);
}
bool is_file(std::string_view path) {
    std::error_code ec;
    return stdfs::is_regular_file(to_path(path), ec);
}
bool is_dir(std::string_view path) {
    std::error_code ec;
    return stdfs::is_directory(to_path(path), ec);
}
bool remove_file(std::string_view path) {
    std::error_code ec;
    return stdfs::remove(to_path(path), ec);
}
std::optional<std::uintmax_t> file_size(std::string_view path) {
    std::error_code ec;
    const auto n = stdfs::file_size(to_path(path), ec);
    if (ec) return std::nullopt;
    return n;
}

std::vector<std::string> list_dir(std::string_view path) {
    std::vector<std::string> out;
    std::error_code ec;
    for (stdfs::directory_iterator it(to_path(path), ec), end; !ec && it != end; it.increment(ec)) {
        out.push_back(from_path(it->path().filename()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string join(std::string_view a, std::string_view b) { return from_path((to_path(a) / to_path(b)).lexically_normal()); }

std::string resolve(std::string_view p) {
    std::error_code ec;
    auto abs = stdfs::absolute(to_path(p), ec);
    if (ec) return std::string(p);
    return from_path(abs.lexically_normal());
}

std::string dirname(std::string_view p) { return from_path(to_path(p).parent_path()); }
std::string basename(std::string_view p) { return from_path(to_path(p).filename()); }
std::string extension_lower(std::string_view p) { return str::to_lower_ascii(from_path(to_path(p).extension())); }

std::optional<std::string> safe_join(std::string_view base, std::string_view rel) {
    if (rel.find('\0') != std::string_view::npos) return std::nullopt;
    const auto r = to_path(rel);
    if (r.is_absolute() || r.has_root_name() || r.has_root_directory()) return std::nullopt;
    for (const auto& part : r.lexically_normal()) {
        if (part == "..") return std::nullopt;
    }
    const auto b = to_path(base).lexically_normal();
    const auto joined = (b / r).lexically_normal();
    const auto relative = joined.lexically_relative(b);
    if (relative.empty()) return std::nullopt;
    for (const auto& part : relative) {
        if (part == "..") return std::nullopt;
    }
    return from_path(joined);
}

} // namespace di::fs
