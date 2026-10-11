void server::register_tool(const tool& tool, tool_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    tools_[tool.name] = std::make_pair(tool, handler);
    
    // Register methods for tool listing and calling
    if (method_handlers_.find("tools/list") == method_handlers_.end()) {
        method_handlers_["tools/list"] = [this](const json& params, const std::string& session_id) -> json {
            json tools_json = json::array();
            for (const auto& [name, tool_pair] : tools_) {
                tools_json.push_back(tool_pair.first.to_json());
            }
            return json{{"tools", tools_json}};
        };
    }
    
    if (method_handlers_.find("tools/call") == method_handlers_.end()) {
        method_handlers_["tools/call"] = [this](const json& params, const std::string& session_id) -> json {
            if (!params.contains("name")) {
                throw mcp_exception(error_code::invalid_params, "Missing 'name' parameter");
            }
            
            std::string tool_name = params["name"];
            auto it = tools_.find(tool_name);
            if (it == tools_.end()) {
                throw mcp_exception(error_code::invalid_params, "Tool not found: " + tool_name);
            }
            
            json tool_args = params.contains("arguments") ? params["arguments"] : json::array();

            if (tool_args.is_string()) {
                try {
                    tool_args = json::parse(tool_args.get<std::string>());
                } catch (const json::exception& e) {
                    throw mcp_exception(error_code::invalid_params, "Invalid JSON arguments: " + std::string(e.what()));
                }
            }

            json tool_result = {
                {"isError", false}
            };

            try {
                tool_result["content"] = it->second.second(tool_args, session_id);
            } catch (const std::exception& e) {
                tool_result["isError"] = true;
                tool_result["content"] = json::array({
                    {
                        {"type", "text"},
                        {"text", e.what()}
                    }
                });
            }

            return tool_result;
        };
    }
}
