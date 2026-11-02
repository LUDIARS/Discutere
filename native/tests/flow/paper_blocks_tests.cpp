// src/flow/paper-blocks.ts の移植の確認 (tests/flow/paper-blocks.test.ts の観点 + golden)。
#include "w1e_fixture.hpp"

#include "flow/paper_blocks.hpp"

using namespace di;
using namespace di::flow;

TEST_CASE("split / join / replace / insert / get match Node") {
    for (const auto& c : w1e::pure()["blocks"]) {
        const std::string md = c["md"].get<std::string>();
        const auto blocks = split_blocks(md);
        Json arr = Json::array();
        for (const auto& b : blocks) arr.push_back(to_json(b));
        CHECK_EQ(w1e::s(arr), w1e::s(c["blocks"]));
        CHECK_EQ(join_blocks(blocks), c["joined"].get<std::string>());
        CHECK_EQ(replace_block(md, "b0", "置換\n\n  "), c["replaced"][0].get<std::string>());
        CHECK_EQ(replace_block(md, "b1", "  "), c["replaced"][1].get<std::string>());
        CHECK_EQ(replace_block(md, "b99", "x"), c["replaced"][2].get<std::string>());
        CHECK_EQ(insert_block_after(md, "b0", "挿入  "), c["inserted"][0].get<std::string>());
        CHECK_EQ(insert_block_after(md, "zz", "末尾に"), c["inserted"][1].get<std::string>());
        CHECK_EQ(insert_block_after(md, "b0", " "), c["inserted"][2].get<std::string>());
        const auto g1 = get_block_text(md, "b1");
        CHECK_EQ(g1 ? Json(*g1) : Json(nullptr), c["got"][0]);
        CHECK(!get_block_text(md, "b42").has_value());
    }
}

TEST_CASE("heading / list / paragraph segmentation (paper-blocks.test.ts)") {
    const auto b = split_blocks("# 議題\n本文\n\n- a\n- b\n段落");
    CHECK_EQ(b.size(), std::size_t{4});
    CHECK_EQ(b[0].type, std::string("heading"));
    CHECK_EQ(b[1].type, std::string("paragraph"));
    CHECK_EQ(b[2].type, std::string("list"));
    CHECK_EQ(b[2].text, std::string("- a\n- b"));
    CHECK_EQ(b[3].id, std::string("b3"));
    // 往復で本文が保たれる
    CHECK_EQ(join_blocks(split_blocks("# a\n\nb\n\n- c")), std::string("# a\n\nb\n\n- c"));
}

TEST_CASE("huge markdown is processed linearly") {
    std::string md;
    for (int i = 0; i < 200'000; ++i) md += (i % 3 == 0 ? "# h\n" : i % 3 == 1 ? "- item\n" : "text\n\n");
    const auto blocks = split_blocks(md);
    CHECK(blocks.size() > 100'000);
    CHECK(!replace_block(md, "b5", "x").empty());
    // 長い 1 行 (空白だらけ) でも落ちない
    CHECK_EQ(split_blocks(std::string(4u << 20, ' ') + "x").size(), std::size_t{1});
}

DI_TEST_MAIN()
