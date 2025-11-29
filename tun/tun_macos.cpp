#include "tun_macos.h"

#ifdef __APPLE__

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <net/if_utun.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/socket.h>
#include <sys/sys_domain.h>
#include <unistd.h>

namespace tun {

TunMacOS::TunMacOS() : fd_(-1), mtu_(1500), unit_number_(-1) {}

TunMacOS::~TunMacOS() { close(); }

bool TunMacOS::open(const std::string &device_name, uint32_t mtu) {
  if (is_open()) {
    last_error_ = "TUN device already open";
    return false;
  }

  // 创建一个内核控制 socket
  fd_ = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
  if (fd_ < 0) {
    last_error_ =
        std::string("Failed to create control socket: ") + strerror(errno);
    return false;
  }

  // 查找 utun 控制 ID
  struct ctl_info info;
  memset(&info, 0, sizeof(info));
  strncpy(info.ctl_name, UTUN_CONTROL_NAME, sizeof(info.ctl_name));

  if (ioctl(fd_, CTLIOCGINFO, &info) < 0) {
    last_error_ = std::string("ioctl CTLIOCGINFO failed: ") + strerror(errno);
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  // 解析设备名称获取单元号（如果提供）
  // 解析设备名称获取单元号（如果提供）
  uint32_t sc_unit = 0; // 0 表示自动分配
  if (!device_name.empty()) {
    int unit = 0;
    if (sscanf(device_name.c_str(), "utun%d", &unit) != 1) {
      last_error_ = "Invalid device name format (expected utunN)";
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    sc_unit = unit + 1;
  }

  // 连接到 utun 控制接口
  struct sockaddr_ctl addr;
  memset(&addr, 0, sizeof(addr));
  addr.sc_len = sizeof(addr);
  addr.sc_family = AF_SYSTEM;
  addr.ss_sysaddr = AF_SYS_CONTROL;
  addr.sc_id = info.ctl_id;
  addr.sc_unit = sc_unit;

  printf("DEBUG: ctl_id=%d, sc_unit=%d\n", info.ctl_id, sc_unit);

  if (connect(fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) <
      0) {
    printf("DEBUG: connect failed, errno=%d (%s)\n", errno, strerror(errno));
    last_error_ =
        std::string("Failed to connect to utun control: ") + strerror(errno);
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  // 获取实际分配的单元号
  char name[64] = {0};
  socklen_t len = sizeof(name);
  if (getsockopt(fd_, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, name, &len) == 0) {
    device_name_ = name;
    if (sscanf(name, "utun%d", &unit_number_) != 1) {
      unit_number_ = -1; // Should not happen
    }
    printf("DEBUG: getsockopt success, name=%s, unit=%d\n", name, unit_number_);
  } else {
    printf("DEBUG: getsockopt failed, errno=%d (%s)\n", errno, strerror(errno));
    // 回退方案：假设使用了请求的单元号
    if (sc_unit > 0) {
      unit_number_ = sc_unit - 1;
      device_name_ = "utun" + std::to_string(unit_number_);
    } else {
      // 如果是自动分配且获取失败，我们不知道具体的单元号
      unit_number_ = -1;
      device_name_ = "";
    }
  }

  if (device_name_.empty()) {
    last_error_ = "Failed to determine TUN device name";
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  mtu_ = mtu;

  return true;
}

void TunMacOS::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  device_name_.clear();
  unit_number_ = -1;
}

bool TunMacOS::is_open() const { return fd_ >= 0; }

std::string TunMacOS::get_device_name() const { return device_name_; }

bool TunMacOS::set_ip(const std::string &ip_address,
                      const std::string &netmask) {
  if (!is_open()) {
    last_error_ = "TUN device not open";
    return false;
  }

  // macOS 上需要使用 ifconfig 命令来配置网络接口
  // 这里使用 system() 调用（不是最优雅的方式，但最可靠）
  std::string cmd = "ifconfig " + device_name_ + " " + ip_address + " " +
                    ip_address + " netmask " + netmask + " mtu " +
                    std::to_string(mtu_);

  if (system(cmd.c_str()) != 0) {
    last_error_ = "Failed to configure interface with ifconfig";
    return false;
  }

  return true;
}

bool TunMacOS::set_up() {
  if (!is_open()) {
    last_error_ = "TUN device not open";
    return false;
  }

  std::string cmd = "ifconfig " + device_name_ + " up";

  if (system(cmd.c_str()) != 0) {
    last_error_ = "Failed to bring interface up";
    return false;
  }

  return true;
}

int TunMacOS::read(uint8_t *buffer, size_t max_length) {
  if (!is_open()) {
    last_error_ = "TUN device not open";
    return -1;
  }

  // macOS 的 utun 在包前面有 4 字节的协议头
  uint8_t temp_buffer[4096];
  ssize_t n = ::read(fd_, temp_buffer, sizeof(temp_buffer));

  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    }
    last_error_ = std::string("Read failed: ") + strerror(errno);
    return -1;
  }

  if (n < 4) {
    last_error_ = "Packet too short";
    return -1;
  }

  // 跳过前 4 字节的协议头
  size_t data_length = n - 4;
  if (data_length > max_length) {
    last_error_ = "Buffer too small";
    return -1;
  }

  memcpy(buffer, temp_buffer + 4, data_length);
  return static_cast<int>(data_length);
}

int TunMacOS::write(const uint8_t *buffer, size_t length) {
  if (!is_open()) {
    last_error_ = "TUN device not open";
    return -1;
  }

  // macOS 的 utun 需要在包前面添加 4 字节的协议头
  uint8_t temp_buffer[4096];
  if (length + 4 > sizeof(temp_buffer)) {
    last_error_ = "Packet too large";
    return -1;
  }

  // 设置协议头（IPv4 = 0x02, IPv6 = 0x1e）
  // 这里假设是 IPv4
  uint32_t protocol = htonl(AF_INET);
  memcpy(temp_buffer, &protocol, 4);
  memcpy(temp_buffer + 4, buffer, length);

  ssize_t n = ::write(fd_, temp_buffer, length + 4);

  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    }
    last_error_ = std::string("Write failed: ") + strerror(errno);
    return -1;
  }

  // 返回实际数据长度（不包括协议头）
  return static_cast<int>(n - 4);
}

std::string TunMacOS::get_last_error() const { return last_error_; }

uint32_t TunMacOS::get_mtu() const { return mtu_; }

bool TunMacOS::set_non_blocking(bool non_blocking) {
  if (!is_open()) {
    last_error_ = "TUN device not open";
    return false;
  }

  int flags = fcntl(fd_, F_GETFL, 0);
  if (flags < 0) {
    last_error_ = std::string("fcntl F_GETFL failed: ") + strerror(errno);
    return false;
  }

  if (non_blocking) {
    flags |= O_NONBLOCK;
  } else {
    flags &= ~O_NONBLOCK;
  }

  if (fcntl(fd_, F_SETFL, flags) < 0) {
    last_error_ = std::string("fcntl F_SETFL failed: ") + strerror(errno);
    return false;
  }

  return true;
}

} // namespace tun

#endif // __APPLE__
