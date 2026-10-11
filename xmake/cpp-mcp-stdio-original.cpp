void server::start_stdio() {
    running_ = true;
    std::string line;
    std::string session_id = "stdio_session_" + std::to_string(std::time(nullptr));
    
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        try {
            json req_json = json::parse(line);
            request req;
            
            if (req_json.is_object() && req_json.contains("jsonrpc") && req_json["jsonrpc"] == "2.0") {
                req.jsonrpc = "2.0";
                if (req_json.contains("id")) {
                    req.id = req_json["id"];
                } else {
                    req.id = nullptr;
                }
                req.method = req_json.value("method", "");
                if (req_json.contains("params")) {
                    req.params = req_json["params"];
                }
                
                json res = process_request(req, session_id);
                if (!res.is_null() && !req.id.is_null()) {
                    std::cout << res.dump() << "\n" << std::flush;
                } else {
                    std::cerr << "Response is null or ID is null. Method: " << req.method << std::endl;
                    if (!res.is_null()) {
                        std::cout << res.dump() << "\n" << std::flush;
                    }
                }
            } else {
                json err_res = {
                    {"jsonrpc", "2.0"},
                    {"error", {
                        {"code", static_cast<int>(error_code::invalid_request)},
                        {"message", "Invalid JSON-RPC format"}
                    }}
                };
                if (req_json.contains("id")) {
                    err_res["id"] = req_json["id"];
                } else {
                    err_res["id"] = nullptr;
                }
                std::cout << err_res.dump() << "\n" << std::flush;
            }
        } catch (const std::exception& e) {
            json err_res = {
                {"jsonrpc", "2.0"},
                {"error", {
                    {"code", static_cast<int>(error_code::parse_error)},
                    {"message", std::string("Parse error: ") + e.what()}
                }},
                {"id", nullptr}
            };
            std::cout << err_res.dump() << "\n" << std::flush;
        }
    }
    running_ = false;
}
