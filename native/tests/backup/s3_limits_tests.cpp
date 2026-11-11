// S3 バックアップの上限 (job cpp-lorica-20261111): part 数・part の範囲・XML 応答の抜き出しの境界。
// 偽の S3 を立てずに済む純関数だけを見る (送受信は s3_part2_tests.cpp)。
#include "check.hpp"

#include <cstdint>
#include <limits>
#include <string>

#include "backup/s3.hpp"

using namespace di::backup;

TEST_CASE("s3_part_count: 上限ちょうど・上限 + 1・0・桁あふれ") {
    CHECK_EQ(s3_part_count(0, 100).value(), std::uint64_t{0});
    CHECK_EQ(s3_part_count(1, 100).value(), std::uint64_t{1});
    CHECK_EQ(s3_part_count(100, 100).value(), std::uint64_t{1});
    CHECK_EQ(s3_part_count(101, 100).value(), std::uint64_t{2});
    // part 数の上限 (10,000) ちょうどは通り、1 つでも超えたら断る
    CHECK_EQ(s3_part_count(std::uint64_t{kMaxS3Parts} * 7, 7).value(), std::uint64_t{kMaxS3Parts});
    CHECK(!s3_part_count(std::uint64_t{kMaxS3Parts} * 7 + 1, 7).has_value());
    CHECK(!s3_part_count(100, 0).has_value());
    // size + part_size - 1 が桁あふれする大きさでも誤った値を返さない
    const auto max = std::numeric_limits<std::uint64_t>::max();
    CHECK(!s3_part_count(max, 2).has_value());
    CHECK_EQ(s3_part_count(max, std::numeric_limits<std::size_t>::max()).value(), std::uint64_t{1});
}

TEST_CASE("s3_part_range: 最後の part は短く、範囲外・桁あふれは断る") {
    const auto first = s3_part_range(1001, 400, 0);
    CHECK(first.has_value());
    CHECK_EQ(first->offset, std::uint64_t{0});
    CHECK_EQ(first->length, std::size_t{400});
    const auto last = s3_part_range(1001, 400, 2);
    CHECK(last.has_value());
    CHECK_EQ(last->offset, std::uint64_t{800});
    CHECK_EQ(last->length, std::size_t{201});
    CHECK(!s3_part_range(1001, 400, 3).has_value());
    CHECK(!s3_part_range(0, 400, 0).has_value());
    CHECK(!s3_part_range(1001, 0, 0).has_value());
    CHECK(!s3_part_range(std::numeric_limits<std::uint64_t>::max(), 1, 0).has_value());
}

TEST_CASE("s3_xml_tag: 正しい応答・壊れた応答・巨大な応答") {
    CHECK_EQ(s3_xml_tag("<R><UploadId>abc</UploadId></R>", "UploadId").value(), std::string("abc"));
    CHECK_EQ(s3_xml_tag("<UploadId></UploadId>", "UploadId").value(), std::string(""));
    CHECK_EQ(s3_xml_tag("<Code>A&amp;B</Code>", "Code").value(), std::string("A&B"));
    CHECK(!s3_xml_tag("<UploadId>abc", "UploadId").has_value());          // 閉じタグが無い
    CHECK(!s3_xml_tag("</UploadId><UploadId>", "UploadId").has_value());  // 順序が逆
    CHECK(!s3_xml_tag("", "UploadId").has_value());
    CHECK(!s3_xml_tag("<Upload", "Upload").has_value());
    // 応答本文の上限 (S3Options::max_response_bytes = 1MiB) ぶんの値も読み切れる (確保は応答の大きさまで)
    const std::string big(1u << 20, 'x');
    CHECK_EQ(s3_xml_tag("<Code>" + big + "</Code>", "Code").value().size(), big.size());
}

DI_TEST_MAIN()
