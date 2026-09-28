#include "token_master.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <chrono>

TokenMasterServer::TokenMasterServer(std::string cfg_path)
    : config_filepath(cfg_path), p1_port(8090), p2_port(9090), heartbeat_interval_min(3),
      instance_id("master_token_01"), is_active_primary(true), running(true), round_robin_idx(0) {}

TokenMasterServer::~TokenMasterServer() { stop(); }
void TokenMasterServer::stop() { running = false; }

bool TokenMasterServer::loadConfiguration()
{
    std::ifstream file(config_filepath);
    std::string line;
    std::unique_lock<std::shared_mutex> lock(master_mutex);
    token_nodes.clear();

    while (std::getline(file, line))
    {
        size_t eq = line.find('=');
        if (eq != std::string::npos)
        {
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            if (k == "P1_PORT")
                p1_port = std::stoi(v);
            else if (k == "P2_PORT")
                p2_port = std::stoi(v);
            else if (k == "HEARTBEAT_INTERVAL_MIN")
                heartbeat_interval_min = std::stoi(v);
            else if (k == "INSTANCE_ID")
                instance_id = v;
            else if (k == "TOKEN_NODES")
            {
                std::stringstream ss(v);
                std::string item;
                while (std::getline(ss, item, ','))
                {
                    size_t c = item.find(':');
                    if (c != std::string::npos)
                    {
                        token_nodes.push_back({item.substr(0, c), std::stoi(item.substr(c + 1))});
                    }
                }
            }
        }
    }
    return true;
}

TokenNodeEndpoint TokenMasterServer::getNextTokenNode()
{
    std::unique_lock<std::shared_mutex> lock(master_mutex);
    if (token_nodes.empty())
        return {"127.0.0.1", 9020};
    auto node = token_nodes[round_robin_idx];
    round_robin_idx = (round_robin_idx + 1) % token_nodes.size();
    return node;
}

void TokenMasterServer::runP1Server()
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(p1_port);

    bind(server_fd, (struct sockaddr *)&address, sizeof(address));
    listen(server_fd, 128);

    std::cout << "[TOKEN MASTER P1] Listening on port " << p1_port << std::endl;

    while (running)
    {
        int client_sock = accept(server_fd, nullptr, nullptr);
        if (client_sock < 0)
            continue;

        std::thread([this, client_sock]()
                    {
            char buffer[4096] = {0};
            int valread = read(client_sock, buffer, sizeof(buffer) - 1);
            if (valread <= 0) {
                close(client_sock);
                return;
            }

            if (!is_active_primary) {
                std::string err = "{\"status\":\"error\",\"message\":\"Master in standby mode\"}";
                send(client_sock, err.c_str(), err.length(), 0);
                close(client_sock);
                return;
            }

            TokenNodeEndpoint node = getNextTokenNode();

            // Forward request to assigned Token Node via TCP
            int node_sock = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in node_addr{};
            node_addr.sin_family = AF_INET;
            node_addr.sin_port = htons(node.port);
            // inet_pton for node.host omitted for brevity, fallback local
            node_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            if (connect(node_sock, (struct sockaddr*)&node_addr, sizeof(node_addr)) == 0) {
                write(node_sock, buffer, valread);
                char node_resp[4096] = {0};
                int n = read(node_sock, node_resp, sizeof(node_resp) - 1);
                if (n > 0) {
                    send(client_sock, node_resp, n, 0);
                }
                close(node_sock);
            } else {
                std::string err = "{\"status\":\"error\",\"message\":\"Token node unavailable\"}";
                send(client_sock, err.c_str(), err.length(), 0);
            }

            close(client_sock); })
            .detach();
    }
    close(server_fd);
}

void TokenMasterServer::runP2Heartbeat()
{
    while (running)
    {
        std::this_thread::sleep_for(std::chrono::minutes(heartbeat_interval_min));
        std::cout << "[TOKEN MASTER P2] [" << instance_id << "] Linked-list heartbeat cluster sync pulse." << std::endl;
    }
}

void TokenMasterServer::start()
{
    if (!loadConfiguration())
        return;
    std::thread(&TokenMasterServer::runP1Server, this).detach();
    runP2Heartbeat();
}

int main()
{
    TokenMasterServer master("config_master.cfg");
    master.start();
    return 0;
}