#pragma once

#include <string>
#include <vector>

#include "base/sqlite.hpp"
#include "playtest/review.hpp"

namespace di::playtest {

struct HumanVoice {
    std::string id;
    std::string gameTitle;
    std::string comment;
};

bool save_ai_review(sql::Database& db, const AiReview& review);
void index_human_playtest_voices(sql::Database& db, const std::vector<HumanVoice>& voices);
std::string ai_review_section(const std::vector<sql::Row>& reviews);
std::string ai_review_section_for_game(sql::Database& db, const std::string& game);
void replace_ai_review_section(std::string& paper, const std::string& section);

} // namespace di::playtest
