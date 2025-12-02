#ifdef __APPLE__

#include "tun_interface.h"

#include <iostream>
#include <sstream>
#include <cstring>
#include <cerrno>

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#include <net/if_utun.h>
#include <arpa/inet.h>
#include <netinet/in.h>

namespace tun {

/**
 * @brief macOS utun 实现
 * 
 * 使用 macOS 内核的 utun 接口
 */
class TunMacOS : public TunInterface {
public:
    TunMacOS();
    ~TunMacOS() override;

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
    int utunNumber_;  // utun 设备编号
};

TunMacOS::TunMacOS()
    : fd_(-1)
    , mtu_(1500)
    , nonBlocking_(false)
    , utunNumber_(-1) {
}

TunMacOS::~TunMacOS() {
    close();
}

void TunMacOS::setError(const std::string& error) {
    lastError_ = error;
    std::cerr << "TUN Error: " << error << std::endl;
}

void TunMacOS::setErrnoError(const std::string& prefix) {
    std::ostringstream oss;
    oss << prefix << ": " << strerror(errno) << " (errno " << errno << ")";
    setError(oss.str());
}

int TunMacOS::netmaskToPrefixLength(const std::string& netmask) {
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

int TunMacOS::runCommand(const std::string& cmd) {
    std::cout << "Executing: " << cmd << std::endl;
    return system(cmd.c_str());
}

bool TunMacOS::open(const std::string& deviceName, int mtu) {
    if (fd_ >= 0) {
        setError("TUN device already open");
        return false;
    }

    // 解析设备名称获取 utun 编号
    // 如果指定了如 "utun5"，则尝试创建 utun5
    // 否则让系统自动分配
    int requestedUnit = -1;  // -1 表示自动分配
    
    if (!deviceName.empty()) {
        if (deviceName.substr(0, 4) == "utun") {
            try {
                requestedUnit = std::stoi(deviceName.substr(4));
            } catch (...) {
                // 解析失败，使用自动分配
            }
        }
    }

    // 创建 PF_SYSTEM socket
    fd_ = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (fd_ < 0) {
        setErrnoError("Failed to create PF_SYSTEM socket");
        return false;
    }

    // 获取 utun 控制 ID
    struct ctl_info ctlInfo;
    memset(&ctlInfo, 0, sizeof(ctlInfo));
    strncpy(ctlInfo.ctl_name, UTUN_CONTROL_NAME, sizeof(ctlInfo.ctl_name) - 1);
    
    if (ioctl(fd_, CTLIOCGINFO, &ctlInfo) < 0) {
        setErrnoError("ioctl CTLIOCGINFO failed");
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // 连接到 utun 控制
    struct sockaddr_ctl sc;
    memset(&sc, 0, sizeof(sc));
    sc.sc_len = sizeof(sc);
    sc.sc_family = AF_SYSTEM;
    sc.ss_sysaddr = AF_SYS_CONTROL;
    sc.sc_id = ctlInfo.ctl_id;
    
    // sc_unit: 0 表示自动分配，其他值表示指定的 utun 编号 + 1
    if (requestedUnit >= 0) {
        sc.sc_unit = requestedUnit + 1;
    } else {
        // 尝试从 0 开始查找可用的 utun
        bool connected = false;
        for (int i = 0; i < 256; i++) {
            sc.sc_unit = i + 1;
            if (connect(fd_, (struct sockaddr*)&sc, sizeof(sc)) == 0) {
                utunNumber_ = i;
                connected = true;
                break;
            }
        }
        
        if (!connected) {
            setErrnoError("Failed to connect to utun control");
            ::close(fd_);
            fd_ = -1;
            return false;
        }
    }
    
    if (requestedUnit >= 0) {
        if (connect(fd_, (struct sockaddr*)&sc, sizeof(sc)) < 0) {
            setErrnoError("Failed to connect to utun control");
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        utunNumber_ = requestedUnit;
    }

    // 构建设备名称
    std::ostringstream oss;
    oss << "utun" << utunNumber_;
    deviceName_ = oss.str();
    mtu_ = mtu;

    std::cout << "TUN device '" << deviceName_ << "' opened successfully" << std::endl;
    return true;
}

void TunMacOS::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    deviceName_.clear();
    utunNumber_ = -1;
}

bool TunMacOS::is_open() const {
    return fd_ >= 0;
}

int TunMacOS::read(uint8_t* buffer, size_t size) {
    if (fd_ < 0) {
        return -1;
    }

    // macOS utun 数据包前面有 4 字节的协议族头
    // 需要读取后跳过这 4 字节
    uint8_t readBuffer[65536];
    size_t readSize = size + 4;
    if (readSize > sizeof(readBuffer)) {
        readSize = sizeof(readBuffer);
    }

    ssize_t n = ::read(fd_, readBuffer, readSize);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 非阻塞模式下没有数据
            return 0;
        }
        setErrnoError("read failed");
        return -1;
    }

    if (n <= 4) {
        // 数据太短
        return 0;
    }

    // 跳过 4 字节协议族头，复制实际数据
    size_t dataSize = n - 4;
    if (dataSize > size) {
        dataSize = size;
    }
    memcpy(buffer, readBuffer + 4, dataSize);

    return static_cast<int>(dataSize);
}

int TunMacOS::write(const uint8_t* buffer, size_t size) {
    if (fd_ < 0) {
        return -1;
    }

    // macOS utun 写入时需要在数据包前面加上 4 字节的协议族头
    uint8_t writeBuffer[65536];
    if (size + 4 > sizeof(writeBuffer)) {
        setError("Packet too large");
        return -1;
    }

    // 检查 IP 版本并设置协议族
    uint32_t af;
    if (size > 0 && (buffer[0] >> 4) == 4) {
        af = htonl(AF_INET);   // IPv4
    } else if (size > 0 && (buffer[0] >> 4) == 6) {
        af = htonl(AF_INET6);  // IPv6
    } else {
        af = htonl(AF_INET);   // 默认 IPv4
    }

    // 构建带协议族头的数据包
    memcpy(writeBuffer, &af, 4);
    memcpy(writeBuffer + 4, buffer, size);

    ssize_t n = ::write(fd_, writeBuffer, size + 4);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 缓冲区满
            return 0;
        }
        setErrnoError("write failed");
        return -1;
    }

    // 返回实际写入的用户数据大小（不含协议族头）
    if (n <= 4) {
        return 0;
    }
    return static_cast<int>(n - 4);
}

std::string TunMacOS::get_device_name() const {
    return deviceName_;
}

bool TunMacOS::set_ip(const std::string& ip, const std::string& netmask) {
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

    // 计算点对点地址（用于 utun）
    // 对于 utun，需要设置本地地址和对端地址
    // 这里简化处理，使用同一子网的 .1 地址作为对端
    uint32_t ipAddr = ntohl(addr.s_addr);
    uint32_t mask = 0xFFFFFFFF << (32 - prefixLength);
    uint32_t network = ipAddr & mask;
    uint32_t peerAddr = network | 1;
    if (peerAddr == ipAddr) {
        peerAddr = network | 2;
    }

    struct in_addr peer;
    peer.s_addr = htonl(peerAddr);
    char peerStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer, peerStr, sizeof(peerStr));

    // 使用 ifconfig 命令设置地址（macOS 风格）
    std::ostringstream cmd;
    cmd << "ifconfig " << deviceName_ << " " << ip << " " << peerStr 
        << " netmask " << netmask << " up";
    
    if (runCommand(cmd.str()) != 0) {
        setError("Failed to set IP address");
        return false;
    }

    std::cout << "Set IP address: " << ip << "/" << prefixLength << std::endl;
    return true;
}

bool TunMacOS::set_mtu(int mtu) {
    if (fd_ < 0 || deviceName_.empty()) {
        setError("TUN device not open");
        return false;
    }

    mtu_ = mtu;

    // 使用 ifconfig 命令设置 MTU
    std::ostringstream cmd;
    cmd << "ifconfig " << deviceName_ << " mtu " << mtu;
    
    if (runCommand(cmd.str()) != 0) {
        setError("Failed to set MTU");
        return false;
    }

    std::cout << "Set MTU: " << mtu << std::endl;
    return true;
}

bool TunMacOS::set_up(bool up) {
    if (fd_ < 0 || deviceName_.empty()) {
        setError("TUN device not open");
        return false;
    }

    // 使用 ifconfig 命令启用/禁用接口
    std::ostringstream cmd;
    cmd << "ifconfig " << deviceName_ << (up ? " up" : " down");
    
    if (runCommand(cmd.str()) != 0) {
        setError("Failed to set interface state");
        return false;
    }

    std::cout << "Interface " << (up ? "enabled" : "disabled") << std::endl;
    return true;
}

bool TunMacOS::set_non_blocking(bool nonBlocking) {
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

std::string TunMacOS::get_last_error() const {
    return lastError_;
}

// 创建 macOS TUN 设备
std::unique_ptr<TunInterface> create_tun() {
    return std::make_unique<TunMacOS>();
}

} // namespace tun

#endif // __APPLE__
