#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <string>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

// 应用程序版本号
#define APP_VERSION_MAJOR 1
#define APP_VERSION_MINOR 0
#define APP_VERSION_PATCH 0
#define APP_VERSION_STRING "1.0.0"

/**
 * @brief 应用程序配置结构
 */
struct AppConfig {
    // 最低版本要求
    std::string min_version = "1.0.0";

    // 应用信息
    struct {
        std::string name = "ConnectTool";
        int steam_app_id = 480;
    } app;

    // VPN 配置
    struct {
        std::string virtual_subnet = "10.0.0.0";
        std::string subnet_mask = "255.0.0.0";
        int default_mtu = 1100;  // 必须小于 Steam unreliable 消息限制(1200) 减去封装开销(35)
        std::string tun_device_name = "SteamVPN";
    } vpn;

    // 协议配置
    struct {
        std::string app_secret_salt = "ConnectTool_VPN_Salt_v1";
        int64_t probe_timeout_ms = 500;
        int64_t heartbeat_interval_ms = 60000;
        int64_t lease_time_ms = 120000;
        int64_t lease_expiry_ms = 360000;
        int64_t heartbeat_expiry_ms = 180000;
        size_t node_id_size = 32;
    } protocol;

    // 网络配置
    struct {
        int send_rate_mb = 50;
        int send_buffer_size_mb = 4;
        int nagle_time = 0;
        int steam_callback_interval_ms = 10;
    } networking;

    // 服务器配置
    struct {
        std::string unix_socket_path_windows = "connect_tool.sock";
        std::string unix_socket_path_unix = "/tmp/connect_tool.sock";
    } server;
};

/**
 * @brief 配置管理器
 * 
 * 单例模式，负责从远程 GitHub 加载配置
 * 支持多个备用 URL，自动重试
 */
class ConfigManager {
public:
    /**
     * @brief 获取配置管理器单例
     */
    static ConfigManager& instance();

    /**
     * @brief 从远程加载配置（同步，依次尝试所有备用 URL）
     * @return true 成功，false 失败（所有 URL 都失败）
     */
    bool loadFromRemote();

    /**
     * @brief 检查应用版本是否满足最低要求
     * @return true 版本满足要求，false 版本过低
     */
    bool checkVersion() const;

    /**
     * @brief 获取最低版本要求
     */
    const std::string& getMinVersion() const { return config_.min_version; }

    /**
     * @brief 获取当前应用版本
     */
    static std::string getAppVersion() { return APP_VERSION_STRING; }

    /**
     * @brief 获取当前配置（只读）
     */
    const AppConfig& getConfig() const;

    /**
     * @brief 获取当前配置（可修改）
     */
    AppConfig& getConfigMutable();

    /**
     * @brief 检查配置是否已加载
     */
    bool isLoaded() const { return loaded_; }

    /**
     * @brief 获取最后一次错误信息
     */
    const std::string& getLastError() const { return lastError_; }

private:
    ConfigManager();
    ~ConfigManager() = default;
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    bool parseJson(const std::string& jsonContent);
    static bool compareVersion(const std::string& appVersion, const std::string& minVersion);

    AppConfig config_;
    mutable std::mutex mutex_;
    bool loaded_ = false;
    std::string lastError_;
    std::vector<std::string> configUrls_;  // 备用 URL 列表
};

#endif // CONFIG_MANAGER_H
