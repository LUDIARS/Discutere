// fuzz の入口を固定の入力で 1 回ずつ呼ぶ回帰用の main (ctest の fuzz.<name>)。libFuzzer を使わないビルド (GCC) 用。
// 引数のディレクトリの各ファイルと、組み込みの境界の入力 (空・全 1 バイト値・巨大な入れ子・巨大な文字列) を渡す。
#include "fuzz_entry.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
int feed(const std::string& input) {
    return LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
}
} // namespace

int main(int argc, char** argv) {
    std::size_t count = 0;
    std::vector<std::string> builtins = {
        "",
        std::string(1u << 20, '['),
        std::string(1u << 20, '{'),
        "\"" + std::string(1u << 20, 'a') + "\"",
        std::string(100'000, '-') + "\n",
        "---\n" + std::string(10'000, ' ') + "a: [" + std::string(5'000, '[') + "\n---\n",
        "<UploadId>" + std::string(1u << 16, '<') + "</UploadId>",
        std::string(24, '\xff'),
    };
    for (int b = 0; b < 256; ++b) builtins.emplace_back(1, static_cast<char>(b));
    for (const auto& input : builtins) {
        feed(input);
        ++count;
    }
    for (int i = 1; i < argc; ++i) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(argv[i], ec)) {
            if (!entry.is_regular_file()) continue;
            std::ifstream in(entry.path(), std::ios::binary);
            const std::string input((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            feed(input);
            ++count;
        }
        if (ec) {
            std::cerr << "corpus not readable: " << argv[i] << "\n";
            return 1;
        }
    }
    std::cout << "fed " << count << " inputs\n";
    return 0;
}
