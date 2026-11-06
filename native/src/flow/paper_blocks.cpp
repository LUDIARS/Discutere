#include "flow/paper_blocks.hpp"

#include "base/strings.hpp"

namespace di::flow {

namespace {

// md.split(/\r?\n/)
std::vector<std::string> split_crlf(std::string_view md) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const auto nl = md.find('\n', start);
        if (nl == std::string_view::npos) {
            out.emplace_back(md.substr(start));
            break;
        }
        std::size_t end = nl;
        if (end > start && md[end - 1] == '\r') --end;
        out.emplace_back(md.substr(start, end - start));
        start = nl + 1;
    }
    return out;
}

bool space_at(std::string_view s, std::size_t i, std::size_t* next) {
    if (i >= s.size()) return false;
    std::size_t j = i;
    const auto cp = str::next_codepoint(s, j);
    if (!str::is_js_space(cp)) return false;
    *next = j;
    return true;
}

std::size_t skip_spaces(std::string_view s, std::size_t i) {
    std::size_t next = i;
    while (space_at(s, i, &next)) i = next;
    return i;
}

// /^#{1,6}\s+/
bool is_heading(std::string_view line) {
    std::size_t k = 0;
    while (k < line.size() && line[k] == '#') ++k;
    if (k < 1 || k > 6) return false;
    std::size_t next = 0;
    return space_at(line, k, &next);
}

// /^(\s*[-*+]\s+|\s*\d+[.)]\s+)/
bool is_list(std::string_view line) {
    std::size_t i = skip_spaces(line, 0);
    if (i >= line.size()) return false;
    std::size_t next = 0;
    if (line[i] == '-' || line[i] == '*' || line[i] == '+') return space_at(line, i + 1, &next);
    std::size_t d = i;
    while (d < line.size() && line[d] >= '0' && line[d] <= '9') ++d;
    if (d == i || d >= line.size() || (line[d] != '.' && line[d] != ')')) return false;
    return space_at(line, d + 1, &next);
}

} // namespace

std::vector<PaperBlock> split_blocks(std::string_view md) {
    struct Buf {
        BlockType type;
        std::vector<std::string> lines;
    };
    std::vector<Buf> blocks;
    std::optional<Buf> buf;
    auto flush = [&] {
        if (buf && !buf->lines.empty()) blocks.push_back(std::move(*buf));
        buf.reset();
    };
    for (const auto& line : split_crlf(md)) {
        if (str::trim(line).empty()) {
            flush();
            continue;
        }
        if (is_heading(line)) {
            flush();
            blocks.push_back({"heading", {line}});
            continue;
        }
        const BlockType type = is_list(line) ? "list" : "paragraph";
        if (!buf || buf->type != type) {
            flush();
            buf = Buf{type, {}};
        }
        buf->lines.push_back(line);
    }
    flush();
    std::vector<PaperBlock> out;
    for (std::size_t i = 0; i < blocks.size(); ++i)
        out.push_back({"b" + std::to_string(i), blocks[i].type, str::join(blocks[i].lines, "\n")});
    return out;
}

std::string join_blocks(const std::vector<std::string>& texts) {
    std::vector<std::string> kept;
    for (const auto& t : texts) {
        std::string trimmed = str::trim_end(t);  // replace(/\s+$/g, "")
        if (!str::trim(trimmed).empty()) kept.push_back(std::move(trimmed));
    }
    return str::join(kept, "\n\n");
}

std::string join_blocks(const std::vector<PaperBlock>& blocks) {
    std::vector<std::string> texts;
    for (const auto& b : blocks) texts.push_back(b.text);
    return join_blocks(texts);
}

std::string replace_block(std::string_view md, std::string_view block_id, std::string_view new_text) {
    auto blocks = split_blocks(md);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (blocks[i].id != block_id) continue;
        if (str::trim(new_text).empty()) blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i));
        else blocks[i].text = str::trim_end(new_text);
        return join_blocks(blocks);
    }
    return std::string(md);
}

std::string insert_block_after(std::string_view md, std::string_view block_id, std::string_view text) {
    if (str::trim(text).empty()) return std::string(md);
    auto blocks = split_blocks(md);
    PaperBlock node{"new", "paragraph", str::trim_end(text)};
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (blocks[i].id == block_id) {
            blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(i + 1), std::move(node));
            return join_blocks(blocks);
        }
    }
    blocks.push_back(std::move(node));
    return join_blocks(blocks);
}

std::optional<std::string> get_block_text(std::string_view md, std::string_view block_id) {
    for (const auto& b : split_blocks(md))
        if (b.id == block_id) return b.text;
    return std::nullopt;
}

Json to_json(const PaperBlock& block) {
    Json o = Json::object();
    o["id"] = block.id;
    o["type"] = block.type;
    o["text"] = block.text;
    return o;
}

} // namespace di::flow
