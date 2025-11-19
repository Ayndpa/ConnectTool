#include "tcp_server.h"
#include "../steam/steam_networking_manager.h"
#include <iostream>
#include <algorithm>

TCPServer::TCPServer(int port, SteamNetworkingManager* manager) : port_(port), running_(false), acceptor_(io_context_), work_(boost::asio::make_work_guard(io_context_)), manager_(manager) {}

TCPServer::~TCPServer() { stop(); }

bool TCPServer::start() {
    try {
        tcp::endpoint endpoint(tcp::v4(), port_);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();

        running_ = true;
        serverThread_ = std::thread([this]() { 
            std::cout << "Server thread started" << std::endl;
            io_context_.run(); 
            std::cout << "Server thread stopped" << std::endl;
        });
        start_accept();
        std::cout << "TCP server started on port " << port_ << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to start TCP server: " << e.what() << std::endl;
        return false;
    }
}

void TCPServer::stop() {
    running_ = false;
    io_context_.stop();
    if (serverThread_.joinable()) {
        serverThread_.join();
    }
    acceptor_.close();
}

void TCPServer::sendToAll(const std::string& message, std::shared_ptr<tcp::socket> excludeSocket) {
    sendToAll(message.c_str(), message.size(), excludeSocket);
}

void TCPServer::sendToAll(const char* data, size_t size, std::shared_ptr<tcp::socket> excludeSocket) {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (auto& client : clients_) {
        if (client != excludeSocket) {
            boost::asio::async_write(*client, boost::asio::buffer(data, size), [](const boost::system::error_code&, std::size_t) {});
        }
    }
}

int TCPServer::getClientCount() {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    return clients_.size();
}

void TCPServer::start_accept() {
    auto socket = std::make_shared<tcp::socket>(io_context_);
    acceptor_.async_accept(*socket, [this, socket](const boost::system::error_code& error) {
        if (!error) {
            std::cout << "New client connected" << std::endl;
            auto multiplexManager = manager_->getMessageHandler()->getMultiplexManager(manager_->getConnection());
            std::string id = multiplexManager->addClient(socket);
            {
                std::lock_guard<std::mutex> lock(clientsMutex_);
                clients_.push_back(socket);
            }
            start_read(socket, id);
        }
        if (running_) {
            start_accept();
        }
    });
}

void TCPServer::start_read(std::shared_ptr<tcp::socket> socket, std::string id) {
    auto buffer = std::make_shared<std::vector<char>>(1024);
    socket->async_read_some(boost::asio::buffer(*buffer), [this, socket, buffer, id](const boost::system::error_code& error, std::size_t bytes_transferred) {
        if (!error) {
            if (manager_->isConnected()) {
                auto multiplexManager = manager_->getMessageHandler()->getMultiplexManager(manager_->getConnection());
                multiplexManager->sendTunnelPacket(id, buffer->data(), bytes_transferred, 0);
            } else {
                std::cout << "Not connected to Steam, skipping forward" << std::endl;
            }
            sendToAll(buffer->data(), bytes_transferred, socket);
            start_read(socket, id);
        } else {
            std::cout << "TCP client " << id << " disconnected or error: " << error.message() << std::endl;
            // Send disconnect packet
            if (manager_->isConnected()) {
                auto multiplexManager = manager_->getMessageHandler()->getMultiplexManager(manager_->getConnection());
                multiplexManager->sendTunnelPacket(id, nullptr, 0, 1);
                // Remove client
                multiplexManager->removeClient(id);
            }
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clients_.erase(std::remove(clients_.begin(), clients_.end(), socket), clients_.end());
        }
    });
}