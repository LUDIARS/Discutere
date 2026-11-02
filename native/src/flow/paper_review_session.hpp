#pragma once
// src/flow/paper-review-session.ts の移植: Discord ペーパーレビューの単一セッション管理 (スレッド単位)。
// ペーパー確定 (承認) までは 1 スレッド = 1 セッション:
//   - preparing: 情報収集〜草案作成中。同じスレッドで準備を二重に始めない。届いた返信は溜める。
//   - reviewing: 草案を出して調整/承認を待っている。
// 返信・承認・自動開始はスレッドごとに直列化する (run_serial)。フロー再提案は 1 セッション 1 回だけ。
// メモリ上の状態だけを持つ (DB・Discord に依存しない)。モジュール内の状態は mutex で守る。
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace di::flow {

/** テスト用: 全セッションとキューを破棄する。 */
void reset_review_sessions_for_test();

/** 準備中セッションを開く。すでにセッションがあれば開かずに false (= 準備を二重に始めない)。 */
bool open_preparing(const std::string& thread_id);
/** 草案を出す前の準備中か。 */
bool is_preparing(const std::string& thread_id);
/** 準備中またはレビュー中のセッションがあるか。 */
bool has_review_session(const std::string& thread_id);
/** 準備中に届いた返信を溜める。準備中でなければ溜めずに false。 */
bool buffer_reply(const std::string& thread_id, const std::string& text);
/** レビュー中へ移し、溜まった返信を取り出す。セッションが無ければレビュー中として開く。 */
std::vector<std::string> mark_reviewing(const std::string& thread_id);
/** フロー再提案を出してよいか。1 セッションにつき最初の 1 回だけ true。 */
bool claim_repropose(const std::string& thread_id);
/** セッションを閉じる (承認・破棄・準備失敗)。溜まった返信も捨てる。 */
void close_review_session(const std::string& thread_id);

namespace detail {
struct SerialQueue;
/** スレッドごとの直列化の順番札 (届いた順に 1 つずつ通す。破棄で次へ渡す)。 */
class SerialTurn {
public:
    explicit SerialTurn(const std::string& thread_id);
    ~SerialTurn();
    SerialTurn(const SerialTurn&) = delete;
    SerialTurn& operator=(const SerialTurn&) = delete;

private:
    std::string thread_id_;
    std::shared_ptr<SerialQueue> queue_;
};
/** テスト用: 直列化の待ち行列がいくつ残っているか (使い終われば 0 に戻る)。 */
std::size_t serial_queue_count();
} // namespace detail

/**
 * スレッドごとに task を直列実行する (同じ thread_id の task は届いた順に 1 つずつ走る)。
 * 前の task が失敗 (例外) しても後続は走る。task の結果を返し、例外は呼び出し側へ投げ直す。
 */
template <class F>
auto run_serial(const std::string& thread_id, F&& task) -> decltype(task()) {
    detail::SerialTurn turn(thread_id);
    return task();
}

} // namespace di::flow
