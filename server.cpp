#include <charconv>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "application.hpp"
#include "third_party/httplib.h"

namespace {
// The snapshot store supports one writer process, not multiple servers.
class WriterLock {
public:
    explicit WriterLock(const std::filesystem::path& path) {
        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd_ < 0) throw std::runtime_error("Cannot open state lock file.");
        if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd_);
            throw std::runtime_error("Another server is already using this state file.");
        }
    }
    ~WriterLock() { ::close(fd_); }
    WriterLock(const WriterLock&) = delete;
    WriterLock& operator=(const WriterLock&) = delete;
private:
    int fd_ = -1;
};
}

int main(int argc, char** argv) {
    try {
        std::filesystem::path state = "data/state.json", web = "web";
        int port = 8080;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--help") {
                std::cout << "Usage: intraday-server [--port 8080] [--state data/state.json] [--web web]\n";
                return 0;
            }
            if (i + 1 >= argc) throw std::invalid_argument("Missing option value.");
            const std::string value = argv[++i];
            if (argument == "--state") state = value;
            else if (argument == "--web") web = value;
            else if (argument == "--port") {
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
                if (error != std::errc{} || end != value.data() + value.size() || port < 1 || port > 65535)
                    throw std::invalid_argument("Port must be from 1 to 65535.");
            } else throw std::invalid_argument("Unknown option: " + argument);
        }
        if (state.has_parent_path()) std::filesystem::create_directories(state.parent_path());
        if (std::filesystem::is_symlink(state)) throw std::runtime_error("State path must not be a symbolic link.");
        state = std::filesystem::absolute(state).lexically_normal();
        WriterLock writer(state.string() + ".lock");
        intraday::Application app(state);
        httplib::Server server;
        server.set_payload_max_length(2 * 1024 * 1024);
        server.set_default_headers({{"Cache-Control", "no-store"}, {"X-Content-Type-Options", "nosniff"},
            {"Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; object-src 'none'; frame-ancestors 'none'"}});
        if (!server.set_mount_point("/", web.string())) throw std::runtime_error("Cannot find web directory.");
        const auto respond = [&](const httplib::Request& request, httplib::Response& response, auto action) {
            if (request.method != "GET" && request.get_header_value("X-Intraday-Client") != "dashboard") {
                response.status = 403;
                response.set_content("{\"error\":\"Missing dashboard client header.\"}", "application/json");
                return;
            }
            try {
                response.set_content(action().dump(), "application/json");
            } catch (const nlohmann::json::exception&) {
                response.status = 400;
                response.set_content("{\"error\":\"Invalid JSON or missing/incorrect fields.\"}", "application/json");
            } catch (const intraday::DomainError& error) {
                switch (error.code) {
                    case intraday::ErrorCode::InvalidInput: response.status = 400; break;
                    case intraday::ErrorCode::NotFound: response.status = 404; break;
                    case intraday::ErrorCode::Conflict: response.status = 409; break;
                }
                intraday::Json body{{"error", error.what()}};
                if (error.existing_rule_id > 0) body["existing_rule_id"] = error.existing_rule_id;
                response.set_content(body.dump(), "application/json");
            } catch (const std::invalid_argument& error) {
                response.status = 400;
                response.set_content(intraday::Json{{"error", error.what()}}.dump(), "application/json");
            } catch (const std::exception& error) {
                std::cerr << "Request failed: " << error.what() << '\n';
                response.status = 500;
                response.set_content("{\"error\":\"Operation could not be saved; previous state is unchanged.\"}", "application/json");
            }
        };
        server.Get("/api/health", [&](const auto& request, auto& response) {
            respond(request, response, [] { return intraday::Json{{"status", "ok"}}; });
        });
        server.Get("/api/state", [&](const auto& request, auto& response) {
            respond(request, response, [&] { return app.view(); });
        });
        server.Post("/api/rules", [&](const auto& request, auto& response) {
            respond(request, response, [&] {
                const auto result = app.create_rule(intraday::Json::parse(request.body));
                response.status = 201;
                return result;
            });
        });
        server.Patch(R"(/api/rules/(\d+))", [&](const auto& request, auto& response) {
            respond(request, response, [&] {
                const std::string id_text = request.matches[1].str();
                int id = 0;
                const auto [end, error] = std::from_chars(id_text.data(), id_text.data() + id_text.size(), id);
                if (error != std::errc{} || end != id_text.data() + id_text.size() || id < 1)
                    throw std::invalid_argument("Invalid rule ID.");
                return app.update_rule(id, intraday::Json::parse(request.body));
            });
        });
        server.Post("/api/queues", [&](const auto& request, auto& response) {
            respond(request, response, [&] {
                const auto result = app.create_queue(intraday::Json::parse(request.body));
                response.status = 201;
                return result;
            });
        });
        server.Post("/api/maintenance/compact", [&](const auto& request, auto& response) {
            respond(request, response, [&] { return app.compact(intraday::Json::parse(request.body)); });
        });
        server.Post("/api/events", [&](const auto& request, auto& response) {
            respond(request, response, [&] { return app.ingest(intraday::Json::parse(request.body)); });
        });
        server.Post("/api/replay", [&](const auto& request, auto& response) {
            respond(request, response, [&] { return app.replay(request.body); });
        });
        server.Post("/api/demo/next", [&](const auto& request, auto& response) {
            respond(request, response, [&] { return app.next_demo(); });
        });
        std::cout << "Intraday server listening on loopback port " << port << '\n'
                  << "State: " << state << "\nOpen the port in your local browser. Ctrl+C stops the server.\n" << std::flush;
        if (!server.listen("127.0.0.1", port)) throw std::runtime_error("Cannot listen on requested port.");
    } catch (const std::exception& error) {
        std::cerr << "Startup failed: " << error.what() << '\n';
        return 1;
    }
}
