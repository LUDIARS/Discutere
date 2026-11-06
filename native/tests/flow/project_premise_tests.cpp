// src/flow/project-premise.ts の移植のテスト (Node の出力と比べる)。
#include "check.hpp"
#include "flow/project_premise.hpp"
#include "w1d_support.hpp"

using namespace di;

TEST_CASE("project premise matches Node") {
    const Json f = w1d::load_fixture("flow/premise.json");
    for (const auto& c : f["premise"])
        CHECK_EQ(flow::project_premise(json_opt_string(c, "input")), c["output"].get<std::string>());
    for (const auto& c : f["requires"])
        CHECK_EQ(flow::requires_project_title(json_opt_string(c, "input")), c["output"].get<bool>());
}

DI_TEST_MAIN()
