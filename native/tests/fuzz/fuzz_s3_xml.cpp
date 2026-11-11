// S3 の XML 応答から値を抜き出す s3_xml_tag と、multipart の part 計算の fuzz の入口。
#include "fuzz_entry.hpp"

#include <cstring>
#include <string>

#include "backup/s3.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string xml(reinterpret_cast<const char*>(data), size);
    (void)di::backup::s3_xml_tag(xml, "UploadId");
    (void)di::backup::s3_xml_tag(xml, "Code");
    if (size >= 24) {  // 先頭 24 バイトを (ファイルの大きさ, part の大きさ, part の番号) として使う
        std::uint64_t file_size = 0, part_size = 0, index = 0;
        std::memcpy(&file_size, data, 8);
        std::memcpy(&part_size, data + 8, 8);
        std::memcpy(&index, data + 16, 8);
        (void)di::backup::s3_part_count(file_size, static_cast<std::size_t>(part_size));
        (void)di::backup::s3_part_range(file_size, static_cast<std::size_t>(part_size), index);
    }
    return 0;
}
