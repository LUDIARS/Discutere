#pragma once
// src/persona-engine/llm/concordia-cost.ts — one-shot の費用を Concordia へ送る (best-effort)。
// 送れなければ logs/cost-one-shot-queue.jsonl に追記する。背景スレッドで送り、呼び出し側を待たせない。
#include <optional>
#include <string>

#include "base/json.hpp"

namespace di::persona_engine {

struct ConcordiaCostOneShot {
    std::string service;
    std::string provider;
    std::string command;
    std::optional<std::string> model;
    std::optional<std::string> cwd;
    std::string prompt;
    std::string status;  // ok / error / timeout / unknown
    std::optional<long long> exit_code;
    std::optional<long long> duration_ms;
    std::optional<double> input_tokens;
    std::optional<double> output_tokens;
    std::optional<double> total_tokens;
    std::optional<double> cost_usd;
    std::optional<Json> metadata;
};

Json concordia_cost_to_json(const ConcordiaCostOneShot& payload);
void report_concordia_cost_one_shot(ConcordiaCostOneShot payload);

} // namespace di::persona_engine
