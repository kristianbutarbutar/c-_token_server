#include "token_node.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <random>
#include <iomanip>
#include <libpq-fe.h>

TokenNodeServer::TokenNodeServer(std::string cfg_path)
    : token_port(9020), heartbeat_interval_min(5), node_id("token_node_01"), running(true)
{

    std::ifstream file(cfg_path);
    std::string line;
    while (std::getline(file, line))
    {
        size_t eq = line.find('=');
        if (eq != std::string::npos)
        {
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            if (k == "TOKEN_PORT")
                token_port = std::stoi(v);
            else if (k == "HEARTBEAT_DB_INTERVAL_MIN")
                heartbeat_interval_min = std::stoi(v);
            else if (k == "DB_CONNECTION")
                db_connection_str = v;
            else if (k == "NODE_ID")
                node_id = v;
        }
    }
}

TokenNodeServer::~TokenNodeServer() { stop(); }
void TokenNodeServer::stop() { running = false; }

std::string TokenNodeServer::generateRandomString(size_t length)
{
    const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::default_random_engine rng{std::random_device{}()};
    std::uniform_int_distribution<size_t> dist(0, sizeof(charset) - 2);
    std::string s;
    for (size_t i = 0; i < length; ++i)
        s += charset[dist(rng)];
    return s;
}

void TokenNodeServer::logRequestToDB(const std::string &action, const std::string &uid, const std::string &details, const std::string &response)
{
    PGconn *conn = PQconnectdb(db_connection_str.c_str());
    if (PQstatus(conn) != CONNECTION_OK)
    {
        PQfinish(conn);
        return;
    }

    const char *create_table =
        "CREATE TABLE IF NOT EXISTS t_token_master ("
        "id SERIAL PRIMARY KEY, "
        "log_timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP, "
        "action VARCHAR(50), "
        "uid VARCHAR(255), "
        "node_assigned VARCHAR(100), "
        "request_details TEXT, "
        "response_payload TEXT);";

    PGresult *res = PQexec(conn, create_table);
    PQclear(res);

    std::string query = "INSERT INTO t_token_master (action, uid, node_assigned, request_details, response_payload) VALUES ($1, $2, $3, $4, $5);";
    const char *vals[5] = {action.c_str(), uid.c_str(), node_id.c_str(), details.c_str(), response.c_str()};
    res = PQexecParams(conn, query.c_str(), 5, NULL, vals, NULL, NULL, 0);
    PQclear(res);
    PQfinish(conn);
}

void TokenNodeServer::runHeartbeatDaemon()
{
    while (running)
    {
        std::this_thread::sleep_for(std::chrono::minutes(heartbeat_interval_min));
        PGconn *conn = PQconnectdb(db_connection_str.c_str());
        if (PQstatus(conn) == CONNECTION_OK)
        {
            std::string query = "INSERT INTO t_token_master (action, uid, node_assigned, request_details, response_payload) VALUES ('heartbeat', 'system', $1, 'Node heartbeat pulse', 'active');";
            const char *vals[1] = {node_id.c_str()};
            PGresult *res = PQexecParams(conn, query.c_str(), 1, NULL, vals, NULL, NULL, 0);
            PQclear(res);
            PQfinish(conn);
        }
    }
}

void TokenNodeServer::run()
{
    std::thread(&TokenNodeServer::runHeartbeatDaemon, this).detach();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(token_port);

    bind(server_fd, (struct sockaddr *)&address, sizeof(address));
    listen(server_fd, 128);

    std::cout << "[TOKEN NODE] Running on port " << token_port << std::endl;

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

            std::string payload(buffer, valread);
            
            auto extractField = [](const std::string& json, const std::string& key) {
                std::string sk = "\"" + key + "\":";
                size_t p = json.find(sk);
                if (p == std::string::npos) return std::string("");
                size_t s = p + sk.length();
                while (s < json.length() && (json[s] == ' ' || json[s] == '"')) s++;
                size_t e = s;
                while (e < json.length() && json[e] != '"' && json[e] != ',' && json[e] != '}') e++;
                return json.substr(s, e - s);
            };

            std::string action = extractField(payload, "action");
            std::string uid = extractField(payload, "uid");

            if (action == "generate_token") {
                int duration = std::stoi(extractField(payload, "duration").empty() ? "60" : extractField(payload, "duration"));
                std::string priv = generateRandomString(50);
                std::string pub = generateRandomString(50);

                auto now = std::chrono::system_clock::now();
                auto expire_tp = now + std::chrono::minutes(duration);

                std::time_t now_c = std::chrono::system_clock::to_time_t(now);
                std::time_t expire_c = std::chrono::system_clock::to_time_t(expire_tp);

                char t1[100], t2[100];
                std::strftime(t1, sizeof(t1), "%Y-%m-%d %H:%M:%S", std::localtime(&now_c));
                std::strftime(t2, sizeof(t2), "%Y-%m-%d %H:%M:%S", std::localtime(&expire_c));

                TokenRecord rec{uid, priv, pub, t1, t2, expire_tp};
                {
                    std::unique_lock<std::shared_mutex> lock(token_map_mutex);
                    tokens_by_private[priv] = rec;
                    tokens_by_public[pub] = rec;
                }

                std::ostringstream resp;
                resp << "{\"uid\":\"" << uid << "\","
                     << "\"private\":\"" << priv << "\","
                     << "\"public\":\"" << pub << "\","
                     << "\"fromtimestamp\":\"" << t1 << "\","
                     << "\"expiretimestamp\":\"" << t2 << "\"}";

                std::string r_str = resp.str();
                send(client_sock, r_str.c_str(), r_str.length(), 0);
                logRequestToDB("generate_token", uid, payload, r_str);

            } else if (action == "check_token_expiration") {
                std::string token = extractField(payload, "token");
                std::string status = "expired";
                std::string expire_time = "";

                {
                    std::shared_lock<std::shared_mutex> lock(token_map_mutex);
                    auto it = tokens_by_private.find(token);
                    if (it == tokens_by_private.end()) it = tokens_by_public.find(token);

                    if (it != tokens_by_public.end()) {
                        expire_time = it->second.expire_timestamp;
                        if (std::chrono::system_clock::now() < it->second.expire_tp) {
                            status = "valid";
                        }
                    }
                }

                std::ostringstream resp;
                resp << "{\"uid\":\"" << uid << "\","
                     << "\"token\":\"" << token << "\","
                     << "\"status\":\"" << status << "\","
                     << "\"expiredtimestamp\":\"" << expire_time << "\"}";

                std::string r_str = resp.str();
                send(client_sock, r_str.c_str(), r_str.length(), 0);
                logRequestToDB("check_token_expiration", uid, payload, r_str);
            }

            close(client_sock); })
            .detach();
    }
    close(server_fd);
}

int main(int argc, char *argv[])
{
    std::string cfg = argc > 1 ? argv[1] : "config_node.cfg";
    TokenNodeServer node(cfg);
    node.run();
    return 0;
}