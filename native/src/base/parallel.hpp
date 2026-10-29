#pragma once
// Promise.all の置き換え: items を最大 max_parallel 本のスレッドで並行に処理し、入力と同じ順で結果を返す。
// 例外は最初のものを投げ直す (Promise.all と同じく 1 つでも失敗したら失敗)。
// LLM 呼び出し (8〜11 秒) を並べるときに使う。上限を必ず付ける (外部 I/O の同時実行数の上限)。
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

namespace di {

template <class T, class Fn>
auto parallel_map(const std::vector<T>& items, Fn&& fn, std::size_t max_parallel = 4)
    -> std::vector<std::invoke_result_t<Fn&, const T&>> {
    using R = std::invoke_result_t<Fn&, const T&>;
    std::vector<std::optional<R>> slots(items.size());
    std::atomic<std::size_t> next{0};
    std::exception_ptr error;
    std::mutex error_mutex;
    auto worker = [&] {
        while (true) {
            const std::size_t i = next.fetch_add(1);
            if (i >= items.size()) return;
            try {
                slots[i].emplace(fn(items[i]));
            } catch (...) {
                std::lock_guard lock(error_mutex);
                if (!error) error = std::current_exception();
            }
        }
    };
    const std::size_t n = std::min<std::size_t>(std::max<std::size_t>(1, max_parallel), items.size());
    std::vector<std::thread> threads;
    for (std::size_t i = 1; i < n; ++i) threads.emplace_back(worker);
    if (n > 0) worker();
    for (auto& t : threads) t.join();
    if (error) std::rethrow_exception(error);
    std::vector<R> out;
    out.reserve(items.size());
    for (auto& s : slots) out.push_back(std::move(*s));
    return out;
}

/** Promise.allSettled 相当: 失敗は nullopt にして全部待つ。 */
template <class T, class Fn>
auto parallel_map_settled(const std::vector<T>& items, Fn&& fn, std::size_t max_parallel = 4)
    -> std::vector<std::optional<std::invoke_result_t<Fn&, const T&>>> {
    using R = std::invoke_result_t<Fn&, const T&>;
    return parallel_map(items, [&](const T& item) -> std::optional<R> {
        try {
            return fn(item);
        } catch (...) {
            return std::nullopt;
        }
    }, max_parallel);
}

} // namespace di
