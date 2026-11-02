// src/flow/paper-review-intent.ts の移植の確認 (tests/flow/paper-review-intent.test.ts の観点 + golden)。
#include "w1e_fixture.hpp"

#include "flow/paper_review_intent.hpp"

using namespace di;
using namespace di::flow;

TEST_CASE("classify matches Node") {
    for (const auto& c : w1e::pure()["intents"])
        CHECK_EQ(w1e::s(to_json(classify_paper_review_intent(c["text"].get<std::string>()))), w1e::s(c["result"]));
}

TEST_CASE("non-instruction replies match Node") {
    for (const auto& c : w1e::pure()["replies"]) {
        const std::string t = c["text"].get<std::string>();
        if (is_non_instruction_reply(t) != c["result"].get<bool>()) throw test::Failure("reply: " + t);
    }
}

TEST_CASE("intent kinds and labels (paper-review-intent.test.ts)") {
    CHECK_EQ(classify_paper_review_intent("外部の声を取り込んで「周回」").kind, std::string("voices"));
    CHECK_EQ(classify_paper_review_intent("類似ゲーム: 「モンスト」「パズドラ」").terms,
             (std::vector<std::string>{"モンスト", "パズドラ"}));
    CHECK_EQ(classify_paper_review_intent("類似ゲームの外部の声を集めて").kind, std::string("voices"));
    CHECK_EQ(classify_paper_review_intent("観点補足を初心者向けに").kind, std::string("adjust"));
    CHECK_EQ(paper_review_intent_label("similar_games"), std::string("類似するゲーム (メカニクスの代替)"));
    CHECK(is_non_instruction_reply("ない"));
    CHECK(!is_non_instruction_reply("メカニクスに周回を追加"));
}

TEST_CASE("letter/number table") {
    CHECK(is_letter_or_number('a'));
    CHECK(is_letter_or_number(U'あ'));
    CHECK(is_letter_or_number(U'漢'));
    CHECK(is_letter_or_number(U'٣'));
    CHECK(!is_letter_or_number('!'));
    CHECK(!is_letter_or_number(0x1F600));
    CHECK(!is_letter_or_number(0x110000));
}

TEST_CASE("huge replies are bounded and linear") {
    std::string big;
    for (int i = 0; i < 100'000; ++i) big += "類似ゲーム「";
    const auto r = classify_paper_review_intent(big);
    CHECK_EQ(r.kind, std::string("similar_games"));
    std::string loose = "似たゲーム:";
    for (int i = 0; i < 100'000; ++i) loose += "モンストとパズドラ、";
    CHECK(!classify_paper_review_intent(loose).terms.empty());
    CHECK(!is_non_instruction_reply(std::string(1u << 20, 'a')));
}

DI_TEST_MAIN()
