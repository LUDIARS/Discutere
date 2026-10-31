#pragma once
// ファイル操作。パスは UTF-8 の文字列で受け渡す (Windows でも to_path で正しく変換する)。
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace di::fs {

std::filesystem::path to_path(std::string_view utf8);
std::string from_path(const std::filesystem::path& p);

/** ファイルを読む。max_bytes を超えるファイルは読まずに nullopt (lorica::input_budget で課金)。 */
std::optional<std::string> read_file(std::string_view path, std::size_t max_bytes = 16u << 20);
/** 一時ファイルに書いてから rename する。親ディレクトリは作る。 */
bool write_file_atomic(std::string_view path, std::string_view data);
bool append_file(std::string_view path, std::string_view data);
bool ensure_dir(std::string_view path);
bool exists(std::string_view path);
bool is_file(std::string_view path);
bool is_dir(std::string_view path);
bool remove_file(std::string_view path);
std::optional<std::uintmax_t> file_size(std::string_view path);
/** ディレクトリ直下の名前 (ファイル・ディレクトリ) を名前順で返す。 */
std::vector<std::string> list_dir(std::string_view path);
/** path.join 相当。 */
std::string join(std::string_view a, std::string_view b);
/** path.resolve(cwd, p) 相当 (正規化つき)。 */
std::string resolve(std::string_view p);
std::string dirname(std::string_view p);
std::string basename(std::string_view p);
std::string extension_lower(std::string_view p);
/**
 * base の下に収まるかを検査して base/rel を返す (パス検査)。
 * ".." による脱出・絶対パス・NUL を含む rel は nullopt。
 */
std::optional<std::string> safe_join(std::string_view base, std::string_view rel);

} // namespace di::fs
