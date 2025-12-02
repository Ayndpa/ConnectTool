#ifdef __linux__

#include "tun_interface.h"

#include <iostream>
#include <sstream>
#include <cstring>
#include <cerrno>

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <arpa/inet.h>
#include <netinet/in.h>

namespace tun {

/**
 * @brief Linux TUN 实现
 * 
 * 使用 /dev/net/tun 设备和 ioctl 系统调用
 */
class TunLinux : public TunInterface {
public:
    TunLinux();
    ~TunLinux() override;

    bool open(const std::string& deviceName, int mtu) override;
    void close() override;
    bool is_open() const override;

    int read(uint8_t* buffer, size_t size) override;
    int write(const uint8_t* buffer, size_t size) override;

    std::string get_device_name() const override;
    bool set_ip(const std::string& ip, const std::string& netmask) override;
    bool set_mtu(int mtu) override;
    bool set_up(bool up) override;
    bool set_non_blocking(bool nonBlocking) override;
    std::string get_last_error() const override;

private:
    void setError(const std::string& error);
    void setErrnoError(const std::string& prefix);
    
    // 子网掩码字符串转 CIDR 前缀长度
    static int netmaskToPrefixLength(const std::string& netmask);
    
    // 执行系统命令
    static int runCommand(const std::string& cmd);

    int fd_;
    std::string deviceName_;
    std::string lastError_;
    int mtu_;
    bool nonBlocking_;
};

TunLinux::TunLinux()
    : fd_(-1)
    , mtu_(1500)
    , nonBlocking_(false) {
}

TunLinux::~TunLinux() {
    close();
}

void TunLinux::setError(const std::string& error) {
    lastError_ = error;
    std::cerr << "TUN Error: " << error << std::endl;
}

void TunLinux::setErrnoError(const std::string& prefix) {
    std::ostringstream oss;
    oss << prefix << ": " << strerror(errno) << " (errno " << errno << ")";
    setError(oss.str());
}

int TunLinux::netmaskToPrefixLength(const std::string& netmask) {
    struct in_addr addr;
    if (inet_pton(AF_INET, netmask.c_str(), &addr) != 1) {
        return 24;  // 默认值
    }
    
    uint32_t mask = ntohl(addr.s_addr);
    int prefix = 0;
    while (mask & 0x80000000) {
        prefix++;
        mask <<= 1;
    }
    return prefix;
}

int TunLinux::runCommand(const std::string& cmd) {
    std::cout << "Executing: " << cmd << std::endl;
    return system(cmd.c_str());
}

bool TunLinux::open(const std::string& deviceName, int mtu) {
    if (fd_ >= 0) {
        setError("TUN device already open");
        return false;
    }

    // 打开 /dev/net/tun
    fd_ = ::open("/dev/net/tun", O_RDWR);
    if (fd_ < 0) {
        setErrnoError("Failed to open /dev/net/tun");
        return false;
    }

    // 配置 TUN 设备
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    
    // IFF_TUN: TUN 设备（三层，IP 数据包）
    // IFF_NO_PI: 不需要包信息头
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

    // 设置设备名称
    std::string name = deviceName.empty() ? "steamvpn%d" : deviceName;
    if (name.length() >= IFNAMSIZ) {
        name = name.substr(0, IFNAMSIZ - 1);
    }
    strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);

    // 创建 TUN 设备
    if (ioctl(fd_, TUNSETIFF, &ifr) < 0) {
        setErrnoError("ioctl TUNSETIFF failed");
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // 保存实际的设备名称
    deviceName_ = ifr.ifr_name;
    mtu_ = mtu;

    std::cout << "TUN device '" << deviceName_ << "' opened successfully" << std::endl;
    return true;
}

void TunLinux::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    deviceName_.clear();
}

bool TunLinux::is_open() const {
    return fd_ >= 0;
}

int TunLinux::read(uint8_t* buffer, size_t size) {
    if (fd_ < 0) {
        return -1;
    }

    ssize_t n = ::read(fd_, buffer, size);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 非阻塞模式下没有数据
            return 0;
        }
        setErrnoError("read failed");
        return -1;
    }

    return static_cast<int>(n);
}

int TunLinux::write(const uint8_t* buffer, size_t size) {
    if (fd_ < 0) {
        return -1;
    }

    ssize_t n = ::write(fd_, buffer, size);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 缓冲区满
            return 0;
        }
        setErrnoError("write failed");
        return -1;
    }

    return static_cast<int>(n);
}

std::string TunLinux::get_device_name() const {
    return deviceName_;
}

bool TunLinux::set_ip(const std::string& ip, const std::string& netmask) {
    if (fd_ < 0 || deviceName_.empty()) {
        setError("TUN device not open");
        return false;
    }

    // 验证 IP 地址格式
    struct in_addr addr;
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
        setError("Invalid IP address: " + ip);
        return false;
    }

    int prefixLength = netmaskToPrefixLength(netmask);

    // 使用 ip 命令设置地址
    std::ostringstream cmd;
    cmd << "ip addr add " << ip << "/" << prefixLength 
        << " dev " << deviceName_;
    
    if (runCommand(cmd.str()) != 0) {
        // 可能地址已存在，尝试先删除再添加
        std::ostringstream delCmd;
        delCmd << "ip addr flush dev " << deviceName_;
        runCommand(delCmd.str());
        
        if (runCommand(cmd.str()) != 0) {
            setError("Failed to set IP address");
            return false;
        }
    }

    std::cout << "Set IP address: " << ip << "/" << prefixLength << std::endl;
    return true;
}

bool TunLinux::set_mtu(int mtu) {
    if (fd_ < 0 || deviceName_.empty()) {
        setError("TUN device not open");
        return false;
    }

    mtu_ = mtu;

    // 使用 ip 命令设置 MTU
    std::ostringstream cmd;
    cmd << "ip link set dev " << deviceName_ << " mtu " << mtu;
    
    if (runCommand(cmd.str()) != 0) {
        setError("Failed to set MTU");
        return false;
    }

    std::cout << "Set MTU: " << mtu << std::endl;
    return true;
}

bool TunLinux::set_up(bool up) {
    if (fd_ < 0 || deviceName_.empty()) {
        setError("TUN device not open");
        return false;
    }

    // 使用 ip 命令启用/禁用接口
    std::ostringstream cmd;
    cmd << "ip link set dev " << deviceName_ << (up ? " up" : " down");
    
    if (runCommand(cmd.str()) != 0) {
        setError("Failed to set interface state");
        return false;
    }

    std::cout << "Interface " << (up ? "enabled" : "disabled") << std::endl;
    return true;
}

bool TunLinux::set_non_blocking(bool nonBlocking) {
    if (fd_ < 0) {
        setError("TUN device not open");
        return false;
    }

    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
        setErrnoError("fcntl F_GETFL failed");
        return false;
    }

    if (nonBlocking) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }

    if (fcntl(fd_, F_SETFL, flags) < 0) {
        setErrnoError("fcntl F_SETFL failed");
        return false;
    }

    nonBlocking_ = nonBlocking;
    return true;
}

std::string TunLinux::get_last_error() const {
    return lastError_;
}

// 创建 Linux TUN 设备
std::unique_ptr<TunInterface> create_tun() {
    return std::make_unique<TunLinux>();
}

} // namespace tun

#endif // __linux__
