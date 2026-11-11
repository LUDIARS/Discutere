// Discord Gateway の受信フレーム (JSON テキスト) → payload の取り出し → 重複排除の鍵、の fuzz の入口。
// GatewayClient の受信と同じ上限 (GatewayClientOptions の既定) で読む。
#include "fuzz_entry.hpp"

#include <string>
#include <string_view>

#include "base/json.hpp"
#include "discord_hook/gateway_client.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const di::discord_hook::GatewayClientOptions defaults;
    if (size > defaults.max_message_bytes) return 0;  // ws 層が read_message_max で断る大きさ
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    auto parsed = di::parse_json(text, defaults.json_limits);
    if (!parsed || !parsed->is_object()) return 0;
    const auto op = di::json_int(*parsed, "op", -1);
    const std::string event = di::json_string(*parsed, "t");
    const di::Json& d = di::json_object(*parsed, "d");
    if (op == di::discord_hook::gateway_op::Dispatch) (void)di::discord_hook::gateway_dedupe_key(event, d);
    (void)di::json_opt_number(d, "heartbeat_interval");
    return 0;
}
