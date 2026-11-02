#include "fundamentum_export/store.hpp"

#include <mutex>

#include "config.hpp"

namespace di::fundamentum_export {

namespace {
std::mutex g_mutex;
std::shared_ptr<lib::fundamentum::Foundation> g_instance;
}  // namespace

lib::fundamentum::Foundation& get_fundamentum_store() {
    std::lock_guard lock(g_mutex);
    if (!g_instance) g_instance = lib::fundamentum::Foundation::on_disk(config::get_config().fundamentum.dataDir);
    return *g_instance;
}

void set_fundamentum_store_for_tests(std::shared_ptr<lib::fundamentum::Foundation> store) {
    std::lock_guard lock(g_mutex);
    g_instance = std::move(store);
}

} // namespace di::fundamentum_export
