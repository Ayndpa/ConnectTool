#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <cstdio>
#include <csignal>

#include <asio.hpp>
#include <grpcpp/grpcpp.h>

#include "protos/connect_tool.grpc.pb.h"
#include "core/connect_tool_core.h"
#include "core/asio_event_loop.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

using connecttool::ConnectToolService;
using connecttool::CreateLobbyRequest;
using connecttool::CreateLobbyResponse;
using connecttool::JoinLobbyRequest;
using connecttool::JoinLobbyResponse;
using connecttool::LeaveLobbyRequest;
using connecttool::LeaveLobbyResponse;
using connecttool::GetLobbyInfoRequest;
using connecttool::GetLobbyInfoResponse;
using connecttool::GetFriendLobbiesRequest;
using connecttool::GetFriendLobbiesResponse;
using connecttool::InviteFriendRequest;
using connecttool::InviteFriendResponse;
using connecttool::GetVPNStatusRequest;
using connecttool::GetVPNStatusResponse;
using connecttool::GetVPNRoutingTableRequest;
using connecttool::GetVPNRoutingTableResponse;

class ConnectToolServiceImpl final : public ConnectToolService::Service {
public:
    ConnectToolServiceImpl(ConnectToolCore* core) : core_(core) {}

    Status CreateLobby(ServerContext* context, const CreateLobbyRequest* request, CreateLobbyResponse* reply) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string lobbyId;
        bool success = core_->createLobby(lobbyId);
        reply->set_success(success);
        reply->set_lobby_id(lobbyId); 
        return Status::OK;
    }

    Status JoinLobby(ServerContext* context, const JoinLobbyRequest* request, JoinLobbyResponse* reply) override {
        std::lock_guard<std::mutex> lock(mutex_);
        bool success = core_->joinLobby(request->lobby_id());
        reply->set_success(success);
        reply->set_message(success ? "Join request sent" : "Failed to join lobby");
        return Status::OK;
    }

    Status LeaveLobby(ServerContext* context, const LeaveLobbyRequest* request, LeaveLobbyResponse* reply) override {
        std::lock_guard<std::mutex> lock(mutex_);
        core_->leaveLobby();
        reply->set_success(true);
        return Status::OK;
    }

    Status GetLobbyInfo(ServerContext* context, const GetLobbyInfoRequest* request, GetLobbyInfoResponse* reply) override {
        std::lock_guard<std::mutex> lock(mutex_);
        bool inLobby = core_->isInLobby();
        reply->set_is_in_lobby(inLobby);
        if (inLobby) {
            reply->set_lobby_id(std::to_string(core_->getCurrentLobbyId().ConvertToUint64()));
            auto members = core_->getLobbyMembers();
            for (const auto& memberID : members) {
                auto* member = reply->add_members();
                member->set_steam_id(std::to_string(memberID.ConvertToUint64()));
                member->set_name(SteamFriends()->GetFriendPersonaName(memberID));
                
                auto connInfo = core_->getMemberConnectionInfo(memberID);
                member->set_ping(connInfo.ping);
                member->set_relay_info(connInfo.relayInfo);
            }
        }
        return Status::OK;
    }

    Status GetFriendLobbies(ServerContext* context, const GetFriendLobbiesRequest* request, GetFriendLobbiesResponse* reply) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto lobbies = core_->getFriendLobbies();
        for (const auto& lobby : lobbies) {
            auto* friendLobby = reply->add_lobbies();
            friendLobby->set_steam_id(std::to_string(lobby.friendID.ConvertToUint64()));
            friendLobby->set_name(lobby.friendName);
            friendLobby->set_lobby_id(std::to_string(lobby.lobbyID.ConvertToUint64()));
        }
        return Status::OK;
    }

    Status InviteFriend(ServerContext* context, const InviteFriendRequest* request, InviteFriendResponse* reply) override {
        std::lock_guard<std::mutex> lock(mutex_);
        bool success = core_->inviteFriend(request->friend_steam_id());
        reply->set_success(success);
        return Status::OK;
    }

    Status GetVPNStatus(ServerContext* context, const GetVPNStatusRequest* request, GetVPNStatusResponse* reply) override {
        std::lock_guard<std::mutex> lock(mutex_);
        reply->set_enabled(core_->isVPNEnabled());
        reply->set_local_ip(core_->getLocalVPNIP());
        reply->set_device_name(core_->getTunDeviceName());
        
        auto stats = core_->getVPNStatistics();
        auto* statsProto = reply->mutable_stats();
        statsProto->set_packets_sent(stats.packetsSent);
        statsProto->set_bytes_sent(stats.bytesSent);
        statsProto->set_packets_received(stats.packetsReceived);
        statsProto->set_bytes_received(stats.bytesReceived);
        statsProto->set_packets_dropped(stats.packetsDropped);
        
        return Status::OK;
    }

    Status GetVPNRoutingTable(ServerContext* context, const GetVPNRoutingTableRequest* request, GetVPNRoutingTableResponse* reply) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto table = core_->getVPNRoutingTable();
        for (const auto& entry : table) {
            auto* route = reply->add_routes();
            route->set_ip(entry.first);
            route->set_name(entry.second.name);
            route->set_is_local(entry.second.isLocal);
        }
        return Status::OK;
    }

private:
    ConnectToolCore* core_;
    std::mutex mutex_;
};

// 信号处理
std::unique_ptr<grpc::Server> g_server;
std::atomic<bool> g_running{true};

void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    g_running = false;
    AsioEventLoop::instance().stop();
    if (g_server) {
        g_server->Shutdown();
    }
}

/**
 * @brief 基于 Asio 的 Steam 回调定时器
 * 
 * 使用 Asio 定时器替代传统的 sleep 循环，实现更高效的事件驱动
 */
class SteamCallbackTimer {
public:
    SteamCallbackTimer(asio::io_context& io, ConnectToolCore* core, std::chrono::milliseconds interval)
        : timer_(io)
        , core_(core)
        , interval_(interval)
        , running_(false) {}

    void start() {
        running_ = true;
        scheduleNext();
    }

    void stop() {
        running_ = false;
        timer_.cancel();
    }

private:
    void scheduleNext() {
        if (!running_) return;
        
        timer_.expires_after(interval_);
        timer_.async_wait([this](const asio::error_code& ec) {
            if (!ec && running_) {
                core_->update();
                scheduleNext();
            }
        });
    }

    asio::steady_timer timer_;
    ConnectToolCore* core_;
    std::chrono::milliseconds interval_;
    std::atomic<bool> running_;
};

int main(int argc, char** argv) {
    // 设置信号处理
#ifdef _WIN32
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
#else
    struct sigaction sa;
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif

    // Initialize Core
    ConnectToolCore core;
    if (!core.initSteam()) {
        std::cerr << "Failed to initialize Steam. Exiting." << std::endl;
        return 1;
    }

    // 获取 Asio 事件循环
    auto& eventLoop = AsioEventLoop::instance();
    auto& ioContext = eventLoop.getContext();

    // 创建 Steam 回调定时器 (10ms 间隔)
    SteamCallbackTimer steamTimer(ioContext, &core, std::chrono::milliseconds(10));
    steamTimer.start();

    // Define server address based on platform
#ifdef _WIN32
    std::string socket_path = "connect_tool.sock";
#else
    std::string socket_path = "/tmp/connect_tool.sock";
#endif

    // Remove the socket file if it already exists
    std::remove(socket_path.c_str());

    std::string server_address("unix:" + socket_path);
    ConnectToolServiceImpl service(&core);

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    g_server = std::unique_ptr<Server>(builder.BuildAndStart());
    
    if (!g_server) {
        std::cerr << "Failed to start gRPC server" << std::endl;
        return 1;
    }
    
    std::cout << "Server listening on " << server_address << std::endl;
    std::cout << "Press Ctrl+C to shutdown..." << std::endl;

    // 在后台线程运行 gRPC 服务器
    std::thread grpcThread([&]() {
        g_server->Wait();
    });

    // 在主线程运行 Asio 事件循环
    eventLoop.run();

    // 清理
    steamTimer.stop();
    
    if (grpcThread.joinable()) {
        grpcThread.join();
    }

    std::cout << "Server shutdown complete." << std::endl;
    return 0;
}
