// cernere_service_token のテスト (3/3)。準備は cernere_service_token_support.hpp。
#include "cernere_service_token_support.hpp"

using namespace cernere_service_token_support;

TEST_CASE("Voluptas pull: URL checks, paging with cursor, redirects refused, page limit") {
    using namespace di::flow;
    std::vector<std::string> auth, urls;
    int page = 0;
    PullVoluptasArgs a;
    a.url = "https://voluptas.test/api/personas/export?limit=5&cursor=old";
    a.token = "fixed";
    a.serviceToken = ServiceTokenProvider([] { return ServiceTokenResult{true, "svc-token", ""}; });
    a.fetchImpl = [&](const http::ClientRequest& req) {
        auth.push_back(*req.headers.get("authorization"));
        urls.push_back(req.url);
        CHECK_EQ(req.max_redirects, 0);
        ++page;
        http::ClientResponse r;
        r.transport_ok = true;
        r.status = 200;
        r.body = page == 1 ? "{\"pseudoId\":\"a\"}\n{bad\n" : "[{\"pseudoId\":\"b\"},{\"pseudoId\":\"c\"}]";
        if (page == 1) r.headers.set("x-next-cursor", "c 2&x");
        return r;
    };
    const auto r = pull_voluptas_personas(a);
    CHECK_EQ(r.pages, 2);
    CHECK_EQ(r.personas.size(), std::size_t{3});
    CHECK_EQ(r.invalidJsonLines, 1);
    CHECK_EQ(auth, (std::vector<std::string>{"Bearer svc-token", "Bearer svc-token"}));
    CHECK_EQ(urls[0], a.url);
    CHECK_EQ(urls[1], std::string("https://voluptas.test/api/personas/export?limit=5&cursor=c+2%26x"));

    for (const char* bad : {"http://voluptas.test/x", "ftp://voluptas.test/x", "http://10.0.0.1/x"}) {
        PullVoluptasArgs b = a;
        b.url = bad;
        CHECK_THROWS(pull_voluptas_personas(b));
    }
    PullVoluptasArgs loop = a;
    loop.url = "http://127.0.0.1:4000/export";
    loop.fetchImpl = [](const http::ClientRequest&) {
        http::ClientResponse r;
        r.transport_ok = true;
        r.status = 200;
        r.headers.set("x-next-cursor", "again");
        return r;
    };
    try {
        pull_voluptas_personas(loop);
        CHECK(false);
    } catch (const std::exception& e) {
        CHECK(std::string(e.what()).find("page limit") != std::string::npos);
    }
    PullVoluptasArgs redirect = a;
    redirect.url = "http://localhost:4000/export";
    redirect.fetchImpl = [](const http::ClientRequest&) {
        http::ClientResponse r;
        r.transport_ok = true;
        r.status = 302;
        r.headers.set("location", "https://elsewhere.test/");
        return r;
    };
    CHECK_THROWS(pull_voluptas_personas(redirect));
}

DI_TEST_MAIN()
