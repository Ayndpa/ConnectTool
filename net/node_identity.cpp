#include "node_identity.h"
#include <cstring>
#include <sstream>
#include <iomanip>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <openssl/sha.h>
#endif

namespace {

#ifdef _WIN32
// Windows 使用 BCrypt API
bool sha256(const uint8_t* data, size_t dataLen, uint8_t* hash) {
    BCRYPT_ALG_HANDLE hAlgorithm = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    NTSTATUS status;
    bool success = false;
    
    status = BCryptOpenAlgorithmProvider(&hAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) return false;
    
    status = BCryptCreateHash(hAlgorithm, &hHash, nullptr, 0, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(hAlgorithm, 0);
        return false;
    }
    
    status = BCryptHashData(hHash, (PUCHAR)data, (ULONG)dataLen, 0);
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptFinishHash(hHash, hash, 32, 0);
        success = BCRYPT_SUCCESS(status);
    }
    
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlgorithm, 0);
    return success;
}
#else
// Linux/macOS 使用 OpenSSL
bool sha256(const uint8_t* data, size_t dataLen, uint8_t* hash) {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, dataLen);
    SHA256_Final(hash, &ctx);
    return true;
}
#endif

} // anonymous namespace

NodeID NodeIdentity::generate(CSteamID steamID) {
    NodeID nodeId;
    
    // 构建输入：SteamID (8字节) + Salt
    uint64_t steamId64 = steamID.ConvertToUint64();
    std::vector<uint8_t> input;
    input.resize(8 + strlen(APP_SECRET_SALT));
    memcpy(input.data(), &steamId64, 8);
    memcpy(input.data() + 8, APP_SECRET_SALT, strlen(APP_SECRET_SALT));
    
    // 计算 SHA-256
    if (!sha256(input.data(), input.size(), nodeId.data())) {
        nodeId.fill(0);
    }
    
    return nodeId;
}

int NodeIdentity::compare(const NodeID& a, const NodeID& b) {
    // 字典序比较（从高位到低位）
    for (size_t i = 0; i < NODE_ID_SIZE; ++i) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

std::string NodeIdentity::toString(const NodeID& nodeId, bool full) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    
    size_t len = full ? NODE_ID_SIZE : 8;
    for (size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<int>(nodeId[i]);
    }
    
    if (!full) {
        oss << "...";
    }
    
    return oss.str();
}

bool NodeIdentity::isEmpty(const NodeID& nodeId) {
    for (size_t i = 0; i < NODE_ID_SIZE; ++i) {
        if (nodeId[i] != 0) return false;
    }
    return true;
}
