#include "discord_hook/directive_handler.hpp"

#include "config.hpp"
#include "core/index.hpp"
#include "core/kg_registry.hpp"
#include "discord_hook/facilitator_directives.hpp"

namespace di::discord_hook {

namespace {

DirectiveMessageResult empty_directive() {
    return {false,
            "🛠️ 進行の調整指示を受け付けます。 例:「もう少し簡単な言葉で話して」「もっと否定的な意見が欲しい」 のように、 "
            "メンションかリプライに続けて書いてください。",
            std::nullopt};
}

DirectiveMessageResult store(sql::Database& kg, const DirectiveMessageInput& input, const std::string& normalized) {
    const auto scene = "discord:" + input.guildId + "/" + input.channelId;
    const auto gap_id = resolve_active_gap_for_scene(kg, input.workspaceId, scene);
    if (!gap_id) {
        return {false,
                "🛠️ いま調整できる進行中の議論が見つかりませんでした。 議論が立ってから、 そのスレッド/チャンネルで指示してください。",
                std::nullopt};
    }
    add_facilitator_directive(kg, {input.workspaceId, *gap_id, normalized, input.authorId});
    return {true, "🛠️ 了解です。「" + normalized + "」 を進行に反映します。 これからの発言とまとめに効いてきます。", gap_id};
}

} // namespace

DirectiveMessageResult handle_directive_message(sql::Database& kg, const DirectiveMessageInput& input) {
    const auto normalized = normalize_directive_text(input.text);
    if (!normalized) return empty_directive();
    return store(kg, input, *normalized);
}

DirectiveMessageResult handle_directive_message(const DirectiveMessageInput& input) {
    // 空文は KG を開かずに返す (TS と同じ順)。
    const auto normalized = normalize_directive_text(input.text);
    if (!normalized) return empty_directive();
    core::CoreOptions opts;
    opts.sqlitePath = core::resolve_active_kg_path(config::get_config());
    auto core = core::create_core(opts);
    return store(core->raw(), input, *normalized);
}

} // namespace di::discord_hook
