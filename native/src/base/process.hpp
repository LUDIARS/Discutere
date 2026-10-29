#pragma once
// 子プロセスの起動。必ず引数配列で起動し、シェル (sh -c / cmd /c) を経由しない。
// 標準出力・標準エラーは上限つき (lorica::input_budget) で読み、期限を過ぎたらプロセスグループごと止める。
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace di::proc {

struct Options {
    /** argv[0] は実行ファイル名 (PATH から探す) またはパス。 */
    std::vector<std::string> argv;
    /** 子に渡す環境変数。nullopt なら親の環境をそのまま渡す。 */
    std::optional<std::map<std::string, std::string>> env;
    /** 作業ディレクトリ (空なら親と同じ)。 */
    std::string cwd;
    /** 標準入力に書く内容 (書き終えたら閉じる)。 */
    std::string stdin_data;
    /** 全体の期限。過ぎたら強制終了して timed_out=true。 */
    std::chrono::milliseconds timeout{120'000};
    std::size_t max_stdout = 16u << 20;
    std::size_t max_stderr = 1u << 20;
    /** 中断の問い合わせ (true を返すと強制終了して cancelled=true)。 */
    std::function<bool()> should_cancel;
};

struct Result {
    bool started = false;
    std::string start_error;
    bool timed_out = false;
    bool cancelled = false;
    /** 出力が上限を超えた (超えた時点で強制終了する)。 */
    bool output_exceeded = false;
    /** 正常終了なら終了コード。シグナル終了・強制終了は -1。 */
    int exit_code = -1;
    std::string out;
    std::string err;
    std::int64_t duration_ms = 0;
};

/** 同期で実行して結果を返す (呼び出したスレッドが待つ)。 */
Result run(const Options& options);

/** PATH (env の PATH を使う) から実行ファイルを探す。見つからなければ nullopt。 */
std::optional<std::string> find_executable(const std::string& name, const std::map<std::string, std::string>& env);

/** Windows の CreateProcess 用に 1 引数を引用する (MSVCRT の規則)。テスト用に公開。 */
std::string quote_windows_arg(const std::string& arg);

} // namespace di::proc
