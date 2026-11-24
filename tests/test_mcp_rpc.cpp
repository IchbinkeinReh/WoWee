// The Model Context Protocol as the server speaks it: the messages, the tools'
// arguments, and what is kept out of the answers.

#include <catch_amalgamated.hpp>

#include <string>

#include "mcp/json_rpc.hpp"
#include "mcp/redact.hpp"
#include "mcp/schema.hpp"

using namespace wowee::mcp;

namespace {

RpcTarget target(int* calls = nullptr) {
    RpcTarget t;
    t.serverVersion = "1";
    t.instructions = "look first";
    t.tools = [] {
        return std::vector<ToolInfo>{
            {.name = "echo",
             .description = "says it back",
             .inputSchema = schema::object({schema::required("text", schema::text("what", 10)),
                                            schema::optional("times", schema::integer("how often", 1, 3))}),
             .readOnly = true},
            {.name = "shot", .description = "a picture", .inputSchema = schema::object({}), .readOnly = true},
            {.name = "fail", .description = "fails", .inputSchema = schema::object({}), .readOnly = false}};
    };
    t.call = [calls](const std::string& name, const json& args) -> ToolAnswer {
        if (calls) ++*calls;
        if (name == "echo") return {.value = args["text"], .failed = false, .imageBase64 = {}, .imageMime = {}};
        if (name == "shot") return {.value = {{"w", 1}}, .failed = false, .imageBase64 = "AAAA", .imageMime = "image/png"};
        return {.value = "no", .failed = true, .imageBase64 = {}, .imageMime = {}};
    };
    return t;
}

json ask(const std::string& body, int* calls = nullptr) {
    const auto answer = answerRpc(body, target(calls));
    REQUIRE(answer);
    return json::parse(*answer);
}

int errorCode(const std::string& body) {
    const json a = ask(body);
    return a.contains("error") ? a["error"]["code"].get<int>() : 0;
}

}  // namespace

TEST_CASE("initialize answers with the version asked for, or the newest", "[mcp][rpc]") {
    const json a = ask(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})");
    CHECK(a["id"] == 1);
    CHECK(a["result"]["protocolVersion"] == "2025-03-26");
    CHECK(a["result"]["serverInfo"]["name"] == "wowee");
    CHECK(a["result"]["capabilities"].contains("tools"));
    CHECK(a["result"]["instructions"] == "look first");
    CHECK(ask(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1999-01-01"}})")
              ["result"]["protocolVersion"] == protocolVersions().front());
    CHECK(ask(R"({"jsonrpc":"2.0","id":1,"method":"initialize"})")["result"]["protocolVersion"] ==
          protocolVersions().front());
    CHECK(ask(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":7}})")
              ["result"]["protocolVersion"] == protocolVersions().front());
}

TEST_CASE("a notification and an answer are not answered", "[mcp][rpc]") {
    CHECK_FALSE(answerRpc(R"({"jsonrpc":"2.0","method":"notifications/initialized"})", target()));
    CHECK_FALSE(answerRpc(R"({"jsonrpc":"2.0","method":"anything at all","params":[1]})", target()));
    CHECK_FALSE(answerRpc(R"({"jsonrpc":"2.0","id":5,"result":{}})", target()));
    CHECK_FALSE(answerRpc(R"([{"jsonrpc":"2.0","method":"a"},{"jsonrpc":"2.0","method":"b"}])", target()));
}

TEST_CASE("ping and the list of tools", "[mcp][rpc]") {
    CHECK(ask(R"({"jsonrpc":"2.0","id":"x","method":"ping"})")["result"] == json::object());
    const json a = ask(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    REQUIRE(a["result"]["tools"].size() == 3);
    CHECK(a["result"]["tools"][0]["name"] == "echo");
    CHECK(a["result"]["tools"][0]["inputSchema"]["type"] == "object");
    CHECK(a["result"]["tools"][0]["annotations"]["readOnlyHint"] == true);
    CHECK(a["result"]["tools"][2]["annotations"]["readOnlyHint"] == false);
}

TEST_CASE("a tool is called with what it takes, and with nothing else", "[mcp][rpc]") {
    int calls = 0;
    const json ok = ask(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hi"}}})",
                        &calls);
    CHECK(ok["result"]["content"][0]["text"] == "hi");
    CHECK(ok["result"]["isError"] == false);
    CHECK(calls == 1);
    for (const char* bad : {R"({"name":"echo"})", R"({"name":"echo","arguments":{}})",
                            R"({"name":"echo","arguments":{"text":1}})", R"({"name":"echo","arguments":{"text":null}})",
                            R"({"name":"echo","arguments":{"text":"12345678901"}})",
                            R"({"name":"echo","arguments":{"text":"a","more":1}})",
                            R"({"name":"echo","arguments":{"text":"a","times":4}})",
                            R"({"name":"echo","arguments":{"text":"a","times":1.5}})",
                            R"({"name":"echo","arguments":["a"]})", R"({"name":"nope","arguments":{}})",
                            R"({"name":7})", R"({})", R"([])"}) {
        INFO(bad);
        const json a =
            ask(std::string(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":)") + bad + "}", &calls);
        CHECK(a["error"]["code"] == -32602);
        CHECK(calls == 1);
    }
}

TEST_CASE("a failure is an answer, and a picture is content", "[mcp][rpc]") {
    const json failed = ask(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"fail"}})");
    CHECK(failed["result"]["isError"] == true);
    CHECK(failed["result"]["content"][0]["text"] == "no");
    const json shot = ask(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"shot","arguments":null}})");
    REQUIRE(shot["result"]["content"].size() == 2);
    CHECK(shot["result"]["content"][0]["text"] == "{\"w\":1}");
    CHECK(shot["result"]["content"][1]["type"] == "image");
    CHECK(shot["result"]["content"][1]["mimeType"] == "image/png");
}

TEST_CASE("malformed messages are told so", "[mcp][rpc]") {
    CHECK(errorCode("") == -32700);
    CHECK(errorCode("{") == -32700);
    CHECK(errorCode("nonsense") == -32700);
    CHECK(errorCode("\xff\xfe") == -32700);
    CHECK(errorCode("1") == -32600);
    CHECK(errorCode("null") == -32600);
    CHECK(errorCode("[]") == -32600);
    CHECK(errorCode(R"({"id":1,"method":"ping"})") == -32600);
    CHECK(errorCode(R"({"jsonrpc":"1.0","id":1,"method":"ping"})") == -32600);
    CHECK(errorCode(R"({"jsonrpc":"2.0","id":{},"method":"ping"})") == -32600);
    CHECK(errorCode(R"({"jsonrpc":"2.0","id":null,"method":"ping"})") == -32600);
    CHECK(errorCode(R"({"jsonrpc":"2.0","id":1})") == -32600);
    CHECK(errorCode(R"({"jsonrpc":"2.0","id":1,"method":7})") == -32600);
    CHECK(errorCode(R"({"jsonrpc":"2.0","id":1,"method":"ping","params":3})") == -32600);
    CHECK(errorCode(R"({"jsonrpc":"2.0","id":1,"method":"resources/read"})") == -32601);
    CHECK(errorCode(std::string(5000, '[')) == -32700);
    CHECK(errorCode("{\"a\":" + std::string(40, '[') + std::string(40, ']') + "}") == -32700);
}

TEST_CASE("a batch is answered message by message", "[mcp][rpc]") {
    const json a = ask(R"([{"jsonrpc":"2.0","id":1,"method":"ping"},{"jsonrpc":"2.0","method":"n"},
                           {"jsonrpc":"2.0","id":2,"method":"nope"},5])");
    REQUIRE(a.is_array());
    REQUIRE(a.size() == 3);
    CHECK(a[0]["id"] == 1);
    CHECK(a[1]["error"]["code"] == -32601);
    CHECK(a[2]["error"]["code"] == -32600);
    std::string many = "[";
    for (int i = 0; i < 17; ++i) many += std::string(i ? "," : "") + R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
    CHECK(errorCode(many + "]") == -32600);
}

TEST_CASE("a tool that throws does not take the server along", "[mcp][rpc]") {
    RpcTarget t = target();
    t.call = [](const std::string&, const json&) -> ToolAnswer { throw std::runtime_error("oh"); };
    const auto a = answerRpc(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"fail"}})", t);
    REQUIRE(a);
    CHECK(json::parse(*a)["error"]["code"] == -32603);
}

TEST_CASE("text that is no UTF-8 is answered all the same", "[mcp][rpc]") {
    RpcTarget t = target();
    t.call = [](const std::string&, const json&) -> ToolAnswer {
        return {.value = {{"name", "\xe4\xff"}}, .failed = false, .imageBase64 = {}, .imageMime = {}};
    };
    const auto a = answerRpc(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"fail"}})", t);
    REQUIRE(a);
    CHECK_NOTHROW(json::parse(*a));
}

TEST_CASE("nesting is counted outside of strings", "[mcp][rpc]") {
    CHECK(nesting("") == 0);
    CHECK(nesting("{}") == 1);
    CHECK(nesting(R"({"a":[{"b":"[[[[\"[["}]})") == 3);
    CHECK(nesting("]]]][") == 1);
}

TEST_CASE("the schemas hold what they say", "[mcp][schema]") {
    const json s = schema::object({schema::required("guid", schema::guid("which")),
                                   schema::optional("n", schema::number("how much", -1, 1)),
                                   schema::optional("f", schema::flag("whether")),
                                   schema::optional("c", schema::choice("one", {"a", "b"})),
                                   schema::optional("l", schema::list("some", json{{"type", "number"}}, 2))});
    const auto ok = [&](const char* args) { return validate(s, json::parse(args)).empty(); };
    CHECK(ok(R"({"guid":"0x1aF"})"));
    CHECK(ok(R"({"guid":"0xF130000C9B00D6E2","n":-1,"f":false,"c":"b","l":[1,2]})"));
    for (const char* bad :
         {R"({})", R"([])", R"("a")", R"({"guid":"1"})", R"({"guid":"0x"})", R"({"guid":"0xg"})",
          R"({"guid":"0x11111111111111111"})", R"({"guid":"0x1","n":1.1})", R"({"guid":"0x1","n":"1"})",
          R"({"guid":"0x1","f":1})", R"({"guid":"0x1","c":"c"})", R"({"guid":"0x1","l":[1,2,3]})",
          R"({"guid":"0x1","l":["1"]})", R"({"guid":"0x1","l":{}})", R"({"guid":"0x1","x":1})"}) {
        INFO(bad);
        CHECK_FALSE(ok(bad));
    }
    CHECK_FALSE(validate(s, json{{"guid", std::string("0x1\0", 4)}}).empty());
    CHECK_FALSE(validate(s, json{{"guid", "0x1"}, {"n", std::numeric_limits<double>::infinity()}}).empty());
}

TEST_CASE("secrets are taken out of every string of an answer", "[mcp][redact]") {
    Redactor r;
    r.add("s3cr3t-token");
    r.add("PlayerOne", true);
    r.add("abc");  // too short to be looked for
    CHECK(r.apply("token s3cr3t-token here, S3CR3T-TOKEN not") == "token [redacted] here, S3CR3T-TOKEN not");
    CHECK(r.apply("as playerone and PLAYERONE, not playerones") ==
          "as [redacted] and [redacted], not playerones");
    CHECK(r.apply("abc") == "abc");
    json j = {{"a", "x s3cr3t-token"}, {"b", {{"c", json::array({"PLAYERONE", 5, nullptr})}}}};
    r.apply(j);
    CHECK(j["a"] == "x [redacted]");
    CHECK(j["b"]["c"][0] == "[redacted]");
    CHECK(j["b"]["c"][1] == 5);
    CHECK(j.dump().find("s3cr3t") == std::string::npos);
    json named = {{"s3cr3t-token", 1}};
    r.apply(named);
    CHECK(named.dump().find("s3cr3t") == std::string::npos);
}

TEST_CASE("a secret inside a longer one leaves nothing of it standing", "[mcp][redact]") {
    Redactor r;
    r.add("word");
    r.add("password-word-more");
    CHECK(r.apply("a password-word-more b") == "a [redacted] b");
}
