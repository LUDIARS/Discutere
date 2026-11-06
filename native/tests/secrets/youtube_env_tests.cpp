// secrets/youtube-env (tests/secrets/youtube-env.test.ts の移植)。
#include "check.hpp"

#include "base/env.hpp"
#include "base/json.hpp"
#include "secrets/youtube_env.hpp"

using namespace di;

TEST_CASE("YouTube credentials come only from injected env, including missing and blank cases") {
    CHECK(!secrets::get_youtube_api_key(secrets::EnvMap{}).has_value());
    CHECK(!secrets::get_youtube_api_key(secrets::EnvMap{{"DISCUTERE_YOUTUBE_API_KEY", "  "}}).has_value());
    CHECK_EQ(secrets::get_youtube_api_key(secrets::EnvMap{{"DISCUTERE_YOUTUBE_API_KEY", " test-key "}}).value(), "test-key");
    CHECK(!secrets::get_youtube_api_key(secrets::EnvMap{{"INFISICAL_CLIENT_SECRET", "unused"}}).has_value());
}

TEST_CASE("status contains presence without exposing the secret") {
    CHECK_EQ(json_stringify(secrets::to_json(secrets::get_youtube_api_key_status(secrets::EnvMap{}))), R"({"present":false})");
    CHECK_EQ(json_stringify(secrets::to_json(secrets::get_youtube_api_key_status(secrets::EnvMap{{"DISCUTERE_YOUTUBE_API_KEY", "  "}}))),
             R"({"present":false})");
    CHECK_EQ(json_stringify(secrets::to_json(secrets::get_youtube_api_key_status(secrets::EnvMap{{"DISCUTERE_YOUTUBE_API_KEY", "test-key"}}))),
             R"({"present":true})");
}

TEST_CASE("process env variant") {
    env::set_for_test("DISCUTERE_YOUTUBE_API_KEY", std::nullopt);
    CHECK(!secrets::get_youtube_api_key().has_value());
    env::set_for_test("DISCUTERE_YOUTUBE_API_KEY", " fake-value ");
    CHECK_EQ(secrets::get_youtube_api_key().value(), "fake-value");
    CHECK(secrets::get_youtube_api_key_status().present);
    env::set_for_test("DISCUTERE_YOUTUBE_API_KEY", std::nullopt);
}

DI_TEST_MAIN()
