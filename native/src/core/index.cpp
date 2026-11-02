#include "core/index.hpp"

namespace di::core {

Core::Core(const CoreOptions& options)
    : client(options.sqlitePath),
      eventLog(client, options.eventsPath.value_or("./data/events.jsonl")),
      repos(repositories::make_context(client, eventLog)) {}

std::unique_ptr<Core> create_core(const CoreOptions& options) { return std::make_unique<Core>(options); }

std::unique_ptr<Core> create_core(const std::string& sqlite_path, const std::optional<std::string>& events_path) {
    return create_core(CoreOptions{sqlite_path, events_path});
}

} // namespace di::core
