// All tools_ access shares mutex_. Copy descriptors/callables under that lock;
// never keep it while formatting a catalogue or invoking application code.
void server::register_tool(const tool& tool, tool_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    tools_[tool.name] = std::make_pair(tool, std::move(handler));
    if (method_handlers_.find("tools/list") == method_handlers_.end()) {
        method_handlers_["tools/list"] = [this](const json&, const std::string&) -> json {
            std::vector<mcp::tool> snapshot;
            {
                std::lock_guard<std::mutex> read_lock(mutex_);
                snapshot.reserve(tools_.size());
                for (const auto& item : tools_) snapshot.push_back(item.second.first);
            }
            json tools_json = json::array();
            for (const auto& descriptor : snapshot) tools_json.push_back(descriptor.to_json());
            return json{{"tools", std::move(tools_json)}};
        };
    }
    if (method_handlers_.find("tools/call") == method_handlers_.end()) {
        method_handlers_["tools/call"] = [this](const json& params, const std::string& session_id) -> json {
            if (!params.contains("name"))
                throw mcp_exception(error_code::invalid_params, "Missing 'name' parameter");
            const std::string name = params["name"];
            tool_handler selected;
            {
                std::lock_guard<std::mutex> read_lock(mutex_);
                const auto found = tools_.find(name);
                if (found == tools_.end())
                    throw mcp_exception(error_code::invalid_params, "Tool not found: " + name);
                selected = found->second.second;
            }
            json arguments = params.contains("arguments") ? params["arguments"] : json::array();
            if (arguments.is_string()) {
                try { arguments = json::parse(arguments.get<std::string>()); }
                catch (const json::exception& e) {
                    throw mcp_exception(error_code::invalid_params, "Invalid JSON arguments: " + std::string(e.what()));
                }
            }
            json result = {{"isError", false}};
            try { result["content"] = selected(arguments, session_id); }
            catch (const std::exception& e) {
                result["isError"] = true;
                result["content"] = json::array({{{"type", "text"}, {"text", e.what()}}});
            }
            return result;
        };
    }
}
