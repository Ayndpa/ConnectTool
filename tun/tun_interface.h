#ifndef TUN_INTERFACE_H
#define TUN_INTERFACE_H

#include <string>
#include <memory>
#include <cstdint>

namespace tun {

/**
 * @brief 虚拟网卡接口
 * 
 * 定义了跨平台的 TUN 设备操作接口
 */
class TunInterface {
public:
    virtual ~TunInterface() = default;

    /**
     * @brief 打开 TUN 设备
     * @param deviceName 设备名称（可选）
     * @param mtu 最大传输单元
     * @return true 成功，false 失败
     */
    virtual bool open(const std::string& deviceName = "", int mtu = 1500) = 0;

    /**
     * @brief 关闭 TUN 设备
     */
    virtual void close() = 0;

    /**
     * @brief 检查设备是否打开
     */
    virtual bool is_open() const = 0;

    /**
     * @brief 读取数据包
     * @param buffer 缓冲区
     * @param size 缓冲区大小
     * @return 读取的字节数，失败返回 -1
     */
    virtual int read(uint8_t* buffer, size_t size) = 0;

    /**
     * @brief 写入数据包
     * @param buffer 数据缓冲区
     * @param size 数据大小
     * @return 写入的字节数，失败返回 -1
     */
    virtual int write(const uint8_t* buffer, size_t size) = 0;

    /**
     * @brief 获取设备名称
     */
    virtual std::string get_device_name() const = 0;

    /**
     * @brief 设置设备 IP 地址和子网掩码
     * @param ip IP 地址字符串（如 "10.0.0.1"）
     * @param netmask 子网掩码字符串（如 "255.255.255.0"）
     * @return true 成功，false 失败
     */
    virtual bool set_ip(const std::string& ip, const std::string& netmask) = 0;

    /**
     * @brief 设置 MTU
     * @param mtu 最大传输单元
     * @return true 成功，false 失败
     */
    virtual bool set_mtu(int mtu) = 0;

    /**
     * @brief 启用/禁用设备
     * @param up true 启用，false 禁用
     * @return true 成功，false 失败
     */
    virtual bool set_up(bool up) = 0;

    /**
     * @brief 设置非阻塞模式
     * @param nonBlocking true 非阻塞，false 阻塞
     * @return true 成功，false 失败
     */
    virtual bool set_non_blocking(bool nonBlocking) = 0;

    /**
     * @brief 获取最后一次错误信息
     */
    virtual std::string get_last_error() const = 0;

    /**
     * @brief 获取读取等待事件句柄（Windows 特有）
     * @return 事件句柄，失败返回 nullptr
     */
    virtual void* get_read_wait_event() const { return nullptr; }
};

/**
 * @brief 创建平台特定的 TUN 设备实例
 * @return TUN 设备智能指针
 */
std::unique_ptr<TunInterface> create_tun();

} // namespace tun

#endif // TUN_INTERFACE_H
