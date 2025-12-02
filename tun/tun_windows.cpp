#ifdef _WIN32

#include "tun_interface.h"
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <rpc.h>
#pragma comment(lib, "Rpcrt4.lib")
#include <iostream>
#include <sstream>
#include <vector>
#include <atomic>
#include <thread>

// WinTUN API 头文件
#include "../third_party/wintun/include/wintun.h"

namespace tun {

// WinTUN DLL 函数指针
static HMODULE g_wintunModule = nullptr;
static WINTUN_CREATE_ADAPTER_FUNC* WintunCreateAdapter = nullptr;
static WINTUN_OPEN_ADAPTER_FUNC* WintunOpenAdapter = nullptr;
static WINTUN_CLOSE_ADAPTER_FUNC* WintunCloseAdapter = nullptr;
static WINTUN_DELETE_DRIVER_FUNC* WintunDeleteDriver = nullptr;
static WINTUN_GET_ADAPTER_LUID_FUNC* WintunGetAdapterLUID = nullptr;
static WINTUN_GET_RUNNING_DRIVER_VERSION_FUNC* WintunGetRunningDriverVersion = nullptr;
static WINTUN_SET_LOGGER_FUNC* WintunSetLogger = nullptr;
static WINTUN_START_SESSION_FUNC* WintunStartSession = nullptr;
static WINTUN_END_SESSION_FUNC* WintunEndSession = nullptr;
static WINTUN_GET_READ_WAIT_EVENT_FUNC* WintunGetReadWaitEvent = nullptr;
static WINTUN_RECEIVE_PACKET_FUNC* WintunReceivePacket = nullptr;
static WINTUN_RELEASE_RECEIVE_PACKET_FUNC* WintunReleaseReceivePacket = nullptr;
static WINTUN_ALLOCATE_SEND_PACKET_FUNC* WintunAllocateSendPacket = nullptr;
static WINTUN_SEND_PACKET_FUNC* WintunSendPacket = nullptr;

// 加载 WinTUN DLL
static bool LoadWintun() {
    if (g_wintunModule) {
        return true;  // 已加载
    }

    // 尝试从 third_party/wintun/bin/amd64 加载
    g_wintunModule = LoadLibraryExW(L"wintun.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_wintunModule) {
        // 尝试从当前目录加载
        g_wintunModule = LoadLibraryW(L"wintun.dll");
    }
    
    if (!g_wintunModule) {
        std::cerr << "Failed to load wintun.dll: " << GetLastError() << std::endl;
        return false;
    }

#define LOAD_PROC(Name) \
    ((Name = reinterpret_cast<decltype(Name)>(GetProcAddress(g_wintunModule, #Name))) == nullptr)

    if (LOAD_PROC(WintunCreateAdapter) ||
        LOAD_PROC(WintunOpenAdapter) ||
        LOAD_PROC(WintunCloseAdapter) ||
        LOAD_PROC(WintunDeleteDriver) ||
        LOAD_PROC(WintunGetAdapterLUID) ||
        LOAD_PROC(WintunGetRunningDriverVersion) ||
        LOAD_PROC(WintunSetLogger) ||
        LOAD_PROC(WintunStartSession) ||
        LOAD_PROC(WintunEndSession) ||
        LOAD_PROC(WintunGetReadWaitEvent) ||
        LOAD_PROC(WintunReceivePacket) ||
        LOAD_PROC(WintunReleaseReceivePacket) ||
        LOAD_PROC(WintunAllocateSendPacket) ||
        LOAD_PROC(WintunSendPacket)) {
        std::cerr << "Failed to get wintun.dll function pointers" << std::endl;
        FreeLibrary(g_wintunModule);
        g_wintunModule = nullptr;
        return false;
    }

#undef LOAD_PROC

    // 设置日志回调
    WintunSetLogger([](WINTUN_LOGGER_LEVEL Level, DWORD64 Timestamp, LPCWSTR Message) {
        const char* levelStr = "INFO";
        switch (Level) {
            case WINTUN_LOG_WARN: levelStr = "WARN"; break;
            case WINTUN_LOG_ERR:  levelStr = "ERR";  break;
            default: break;
        }
        std::wcout << L"[WinTUN " << levelStr << L"] " << Message << std::endl;
    });

    DWORD version = WintunGetRunningDriverVersion();
    if (version == 0) {
        std::cout << "WinTUN driver not running, will be loaded on first adapter creation" << std::endl;
    } else {
        std::cout << "WinTUN driver version: " << ((version >> 16) & 0xFF) << "."
                  << ((version >> 8) & 0xFF) << "." << (version & 0xFF) << std::endl;
    }

    return true;
}

/**
 * @brief Windows WinTUN 实现
 */
class TunWindows : public TunInterface {
public:
    TunWindows();
    ~TunWindows() override;

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
    void* get_read_wait_event() const override;

private:
    // 将字符串转换为宽字符串
    static std::wstring StringToWString(const std::string& str);

    // 设置错误信息
    void setError(const std::string& error);
    void setWindowsError(const std::string& prefix);

    // 子网掩码字符串转 CIDR 前缀长度
    static int NetmaskToPrefixLength(const std::string& netmask);

    WINTUN_ADAPTER_HANDLE adapter_;
    WINTUN_SESSION_HANDLE session_;
    std::string deviceName_;
    std::string lastError_;
    int mtu_;
    bool nonBlocking_;
    NET_LUID adapterLuid_;
    
    // 用于非阻塞读取
    std::atomic<bool> readReady_;
};

TunWindows::TunWindows()
    : adapter_(nullptr)
    , session_(nullptr)
    , mtu_(1500)
    , nonBlocking_(false)
    , readReady_(false) {
    memset(&adapterLuid_, 0, sizeof(adapterLuid_));
}

TunWindows::~TunWindows() {
    close();
}

std::wstring TunWindows::StringToWString(const std::string& str) {
    if (str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring result(size - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], size);
    return result;
}

void TunWindows::setError(const std::string& error) {
    lastError_ = error;
    std::cerr << "TUN Error: " << error << std::endl;
}

void TunWindows::setWindowsError(const std::string& prefix) {
    DWORD error = GetLastError();
    char* msgBuf = nullptr;
    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&msgBuf, 0, nullptr);
    
    std::ostringstream oss;
    oss << prefix << " (Error " << error << ")";
    if (msgBuf) {
        oss << ": " << msgBuf;
        LocalFree(msgBuf);
    }
    setError(oss.str());
}

int TunWindows::NetmaskToPrefixLength(const std::string& netmask) {
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

bool TunWindows::open(const std::string& deviceName, int mtu) {
    if (adapter_) {
        setError("Adapter already open");
        return false;
    }

    // 加载 WinTUN DLL
    if (!LoadWintun()) {
        setError("Failed to load WinTUN library");
        return false;
    }

    mtu_ = mtu;
    
    // 使用提供的名称或默认名称
    std::string name = deviceName.empty() ? "SteamVPN" : deviceName;
    deviceName_ = name;
    
    std::wstring wName = StringToWString(name);
    std::wstring wTunnelType = L"SteamConnectTool";

    // 生成一个固定的 GUID 以保持适配器持久化
    // 使用基于名称的 GUID
    GUID requestedGuid = {0};
    UuidFromStringW((RPC_WSTR)L"e5a3b5c9-8d7e-4f1a-b2c3-d4e5f6a7b8c9", &requestedGuid);

    // 首先尝试打开已存在的适配器
    adapter_ = WintunOpenAdapter(wName.c_str());
    if (!adapter_) {
        // 创建新适配器
        adapter_ = WintunCreateAdapter(wName.c_str(), wTunnelType.c_str(), &requestedGuid);
    }

    if (!adapter_) {
        setWindowsError("Failed to create/open WinTUN adapter");
        return false;
    }

    // 获取适配器 LUID
    WintunGetAdapterLUID(adapter_, &adapterLuid_);

    // 启动会话
    // 使用 512KB 环形缓冲区
    session_ = WintunStartSession(adapter_, 0x80000);
    if (!session_) {
        setWindowsError("Failed to start WinTUN session");
        WintunCloseAdapter(adapter_);
        adapter_ = nullptr;
        return false;
    }

    std::cout << "WinTUN adapter '" << name << "' opened successfully" << std::endl;
    return true;
}

void TunWindows::close() {
    if (session_) {
        WintunEndSession(session_);
        session_ = nullptr;
    }
    
    if (adapter_) {
        WintunCloseAdapter(adapter_);
        adapter_ = nullptr;
    }

    deviceName_.clear();
}

bool TunWindows::is_open() const {
    return adapter_ != nullptr && session_ != nullptr;
}

int TunWindows::read(uint8_t* buffer, size_t size) {
    if (!session_) {
        return -1;
    }

    DWORD packetSize;
    BYTE* packet = WintunReceivePacket(session_, &packetSize);
    
    if (!packet) {
        DWORD error = GetLastError();
        if (error == ERROR_NO_MORE_ITEMS) {
            // 非阻塞模式下没有数据
            if (nonBlocking_) {
                return 0;
            }
            // 阻塞模式下等待数据
            HANDLE event = WintunGetReadWaitEvent(session_);
            if (event) {
                WaitForSingleObject(event, 100);  // 等待最多 100ms
            }
            return 0;
        } else if (error == ERROR_HANDLE_EOF) {
            // 适配器正在关闭
            return -1;
        }
        return -1;
    }

    // 复制数据
    DWORD copySize = (packetSize < size) ? packetSize : static_cast<DWORD>(size);
    memcpy(buffer, packet, copySize);

    // 释放数据包
    WintunReleaseReceivePacket(session_, packet);

    return static_cast<int>(copySize);
}

int TunWindows::write(const uint8_t* buffer, size_t size) {
    if (!session_) {
        return -1;
    }

    if (size > WINTUN_MAX_IP_PACKET_SIZE) {
        setError("Packet too large");
        return -1;
    }

    // 分配发送缓冲区
    BYTE* packet = WintunAllocateSendPacket(session_, static_cast<DWORD>(size));
    if (!packet) {
        DWORD error = GetLastError();
        if (error == ERROR_BUFFER_OVERFLOW) {
            // 缓冲区满，丢弃数据包
            return 0;
        }
        return -1;
    }

    // 复制数据
    memcpy(packet, buffer, size);

    // 发送数据包
    WintunSendPacket(session_, packet);

    return static_cast<int>(size);
}

std::string TunWindows::get_device_name() const {
    return deviceName_;
}

bool TunWindows::set_ip(const std::string& ip, const std::string& netmask) {
    if (!adapter_) {
        setError("Adapter not open");
        return false;
    }

    // 解析 IP 地址
    struct in_addr addr;
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
        setError("Invalid IP address: " + ip);
        return false;
    }

    // 获取前缀长度
    int prefixLength = NetmaskToPrefixLength(netmask);

    // 使用 CreateUnicastIpAddressEntry 添加 IP 地址
    MIB_UNICASTIPADDRESS_ROW addressRow;
    InitializeUnicastIpAddressEntry(&addressRow);
    
    addressRow.InterfaceLuid = adapterLuid_;
    addressRow.Address.Ipv4.sin_family = AF_INET;
    addressRow.Address.Ipv4.sin_addr = addr;
    addressRow.OnLinkPrefixLength = static_cast<UINT8>(prefixLength);
    addressRow.DadState = IpDadStatePreferred;

    // 删除旧的 IP 地址（如果存在）
    DeleteUnicastIpAddressEntry(&addressRow);

    // 添加新的 IP 地址
    DWORD result = CreateUnicastIpAddressEntry(&addressRow);
    if (result != NO_ERROR && result != ERROR_OBJECT_ALREADY_EXISTS) {
        std::ostringstream oss;
        oss << "Failed to set IP address (Error " << result << ")";
        setError(oss.str());
        return false;
    }

    std::cout << "Set IP address: " << ip << "/" << prefixLength << std::endl;
    return true;
}

bool TunWindows::set_mtu(int mtu) {
    if (!adapter_) {
        setError("Adapter not open");
        return false;
    }

    mtu_ = mtu;

    // 使用 netsh 命令设置 MTU
    std::ostringstream cmd;
    cmd << "netsh interface ipv4 set subinterface \"" << deviceName_ 
        << "\" mtu=" << mtu << " store=persistent";
    
    int result = system(cmd.str().c_str());
    if (result != 0) {
        setError("Failed to set MTU via netsh");
        return false;
    }

    std::cout << "Set MTU: " << mtu << std::endl;
    return true;
}

bool TunWindows::set_up(bool up) {
    if (!adapter_) {
        setError("Adapter not open");
        return false;
    }

    // 使用 netsh 命令启用/禁用接口
    std::ostringstream cmd;
    cmd << "netsh interface set interface \"" << deviceName_ << "\" " 
        << (up ? "enable" : "disable");
    
    int result = system(cmd.str().c_str());
    if (result != 0) {
        setError("Failed to set interface state via netsh");
        return false;
    }

    std::cout << "Interface " << (up ? "enabled" : "disabled") << std::endl;
    return true;
}

bool TunWindows::set_non_blocking(bool nonBlocking) {
    nonBlocking_ = nonBlocking;
    return true;
}

std::string TunWindows::get_last_error() const {
    return lastError_;
}

void* TunWindows::get_read_wait_event() const {
    if (session_) {
        return WintunGetReadWaitEvent(session_);
    }
    return nullptr;
}

// 创建 Windows TUN 设备
std::unique_ptr<TunInterface> create_tun() {
    return std::make_unique<TunWindows>();
}

} // namespace tun

#endif // _WIN32
