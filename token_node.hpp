#ifndef TOKEN_NODE_HPP
#define TOKEN_NODE_HPP

#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <atomic>
#include <memory>
#include <chrono>

struct TokenRecord
{
    std::string uid;
    std::string private_token;
    std::string public_token;
    std::string from_timestamp;
    std::string expire_timestamp;
    std::chrono::system_clock::time_point expire_tp;
};

class TokenNodeServer
{
private:
    int token_port;
    int heartbeat_interval_min;
    std::string db_connection_str;
    std::string node_id;
    std::atomic<bool> running;

    mutable std::shared_mutex token_map_mutex;
    std::unordered_map<string, TokenRecord> tokens_by_private;
    std::unordered_map<string, TokenRecord> tokens_by_public;

    std::string generateRandomString(size_t length);
    void logRequestToDB(const std::string &action, const std::string &uid, const std::string &details, const std::string &response);
    void runHeartbeatDaemon();

public:
    explicit TokenNodeServer(std::string cfg_path);
    ~TokenNodeServer();

    void run();
    void stop();
};

#endif