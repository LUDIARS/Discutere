#pragma once
// HTTP の要求・応答とルータ (Hono の置き換え)。Boost には依存しない。
//
// ルータの意味は Hono に合わせる:
//   - 登録順にミドルウェアとルートを並べ、要求に一致するものを順に連鎖させる。
//   - ミドルウェアは next() で後続へ進む。ルートのハンドラは応答を返して連鎖を終える。
//   - パターンは "/a/:id"、末尾 "*" (前方一致)。"/" 区切りの完全一致 (末尾スラッシュは区別)。
//   - mount(prefix, sub) は app.route(prefix, sub) と同じく sub の登録をそのまま prefix 付きで写す。
//   - HEAD は GET のルートで処理し、本文を捨てる。
//   - 一致するルートが無ければ 404 "404 Not Found" (text/plain)。
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.hpp"

namespace di::http {

class Headers {
public:
    /** 大文字小文字を無視して最初の値。 */
    std::optional<std::string> get(std::string_view name) const;
    bool has(std::string_view name) const;
    /** 同名を消してから入れる。 */
    void set(std::string name, std::string value);
    void add(std::string name, std::string value);
    void remove(std::string_view name);
    const std::vector<std::pair<std::string, std::string>>& items() const { return items_; }

private:
    std::vector<std::pair<std::string, std::string>> items_;
};

struct Request {
    std::string method;          // 大文字
    std::string target;          // 要求行のまま ("/a?b=1")
    std::string path;            // デコードしないパス
    std::string query_string;    // "?" を除いたクエリ
    Headers headers;
    std::string body;
    std::string remote_address;  // 相手の IP (文字列)
    std::map<std::string, std::string> params;  // ルートの :name
    std::map<std::string, std::string> vars;    // c.set / c.get

    /** c.req.query(name) (最初の値、デコード済み)。 */
    std::optional<std::string> query(std::string_view name) const;
    std::vector<std::string> queries(std::string_view name) const;
    /** c.req.header(name) */
    std::optional<std::string> header(std::string_view name) const;
    /** c.req.param(name) (デコード済み。無ければ空)。 */
    std::string param(std::string_view name) const;
    /** c.req.json() — 本文を上限つきで読む。壊れていれば nullopt。 */
    std::optional<Json> json(const JsonLimits& limits = {}) const;
    /** 相手が loopback (127.0.0.1 / ::1 / ::ffff:127.0.0.1) か (middleware/auth の isLoopbackRequest)。 */
    bool from_loopback() const;
    std::string var(std::string_view name) const;
};

struct Response {
    int status = 200;
    Headers headers;
    std::string body;

    /** c.json(value, status) — Content-Type: application/json */
    static Response json(const Json& value, int status = 200);
    /** c.html(text, status) — Content-Type: text/html; charset=UTF-8 */
    static Response html(std::string text, int status = 200);
    /** c.text(text, status) — Content-Type: text/plain; charset=UTF-8 */
    static Response text(std::string text, int status = 200);
    static Response redirect(std::string location, int status = 302);
    static Response bytes(std::string data, std::string content_type, int status = 200);
    static Response empty(int status);
};

using Handler = std::function<Response(Request&)>;
using Next = std::function<Response()>;
using Middleware = std::function<Response(Request&, const Next&)>;

/** HTTP 本文の既定上限 (Hono には無かったが、外部入力の課金として全経路に課す)。 */
inline constexpr std::size_t kDefaultMaxBody = 1u << 20;

struct RouteOptions {
    std::size_t max_body = kDefaultMaxBody;
};

class Router {
public:
    void get(std::string pattern, Handler handler, RouteOptions options = {});
    void post(std::string pattern, Handler handler, RouteOptions options = {});
    void put(std::string pattern, Handler handler, RouteOptions options = {});
    void del(std::string pattern, Handler handler, RouteOptions options = {});
    void patch(std::string pattern, Handler handler, RouteOptions options = {});
    /** method="*" で全メソッド。 */
    void on(std::string method, std::string pattern, Handler handler, RouteOptions options = {});
    /** app.use(pattern, mw)。pattern 省略は "*"。 */
    void use(std::string pattern, Middleware middleware);
    void use(Middleware middleware) { use("*", std::move(middleware)); }
    /** 本文上限を指定パターンに課す (Hono の bodyLimit ミドルウェア相当)。 */
    void body_limit(std::string pattern, std::size_t max_body);
    /** app.route(prefix, sub) */
    void mount(std::string_view prefix, const Router& sub);

    /** 要求を処理する。 */
    Response handle(Request& request) const;
    /** 本文を読む前に、この要求に許す本文の上限を返す。 */
    std::size_t max_body_for(std::string_view method, std::string_view path) const;

    /** パターン一致 (テスト用に公開)。params に :name を入れる。 */
    static bool match(std::string_view pattern, std::string_view path, std::map<std::string, std::string>* params);

private:
    struct Entry {
        enum class Kind { route, middleware, limit } kind;
        std::string method;  // route のみ ("*" = 全部)
        std::string pattern;
        Handler handler;
        Middleware middleware;
        std::size_t max_body = kDefaultMaxBody;
    };
    std::vector<Entry> entries_;
};

/** Hono の cors({origin, credentials, allowMethods, allowHeaders}) と同じ挙動のミドルウェア。 */
Middleware cors(std::string origin, bool credentials, std::vector<std::string> allow_methods,
                std::vector<std::string> allow_headers);

} // namespace di::http
