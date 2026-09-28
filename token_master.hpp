#ifndef TOKEN_MASTER_HPP
#define TOKEN_MASTER_HPP

#include <string>
#include <vector>
#include <shared_mutex>
#include <atomic>
#include <thread>

struct TokenMasterNodePeer
{
    std::string instance_id;
    std::string host;
    int p2_port;
    bool is_active;
    TokenMasterNodePeer *next;
};

struct TokenNodeEndpoint
{
    std::string host;
    int port;
};

class TokenMasterServer
{
private:
    std::string config_filepath;
    int p1_port;
    int p2_port;
    int heartbeat_interval_min;
    std::string instance_id;
    std::atomic<bool> is_active_primary;
    std::atomic<bool> running;

    std::vector<TokenNodeEndpoint> token_nodes;
    size_t round_robin_idx;
    mutable std::shared_mutex master_mutex;

    bool loadConfiguration();
    void runP1Server();
    void runP2Heartbeat();
    TokenNodeEndpoint getNextTokenNode();

public:
    explicit TokenMasterServer(std::string cfg_path);
    ~TokenMasterServer();
    void start();
    void stop();
};

#endif