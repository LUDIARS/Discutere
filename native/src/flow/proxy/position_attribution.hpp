#pragma once
// src/flow/proxy/position-attribution.ts の移植: 代理の定立 (Position) の論拠ごとの帰属検査 (S3)。
// 検査に通らない論拠は「推測」に落とし、露出文に帰属の印を付ける。代理でなければ入力をそのまま返す。
#include "flow/dialectic/position.hpp"
#include "flow/effect_predict.hpp"
#include "flow/personas.hpp"

namespace di::flow::proxy {

dialectic::GeneratedPosition attribute_position_grounds(const dialectic::GeneratedPosition& gp, const FlowPersona& persona,
                                                        const WarnFn& warn = {});

} // namespace di::flow::proxy
