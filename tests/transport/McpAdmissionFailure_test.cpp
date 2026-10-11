#include "test_framework.h"
#include "mcp_server.h"

#include <stdexcept>

TEST_CASE("throwing embedding predicate refuses public initialize without publishing a session")
{
	mcp::server::configuration config;
	config.host = "127.0.0.1"; config.port = 37393; config.threadpool_size = 2;
	config.request_admission = []() -> bool { throw std::runtime_error("injected predicate failure"); };
	mcp::server server(config);
	if (!server.start(false)) throw std::runtime_error("owned admission fixture did not acquire its listener");
	httplib::Client client("127.0.0.1", 37393);
	client.set_connection_timeout(1, 0); client.set_read_timeout(2, 0);
	auto reply = client.Post("/mcp", R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})", "application/json");
	CHECK(reply && reply->status == 503);
	if (reply) {
		CHECK(reply->get_header_value("Mcp-Session-Id").empty());
		CHECK(mcp::json::parse(reply->body).at("error") == "Server not ready");
	}
	server.stop();
}
