#ifndef NODE_IDENTITY_H
#define NODE_IDENTITY_H

#include "vpn_protocol.h"
#include <steam_api.h>
#include <string>

/**
 * @brief Node ID 工具类
 * 
 * 提供 Node ID 的生成、比较和转换功能
 */
class NodeIdentity {
public:
    /**
     * @brief 生成 Node ID（SHA-256(SteamID + Salt)）
     * @param steamID Steam ID
     * @return 256位 Node ID
     */
    static NodeID generate(CSteamID steamID);
    
    /**
     * @brief 比较两个 Node ID
     * @param a 第一个 Node ID
     * @param b 第二个 Node ID
     * @return -1 如果 a < b, 0 如果 a == b, 1 如果 a > b
     */
    static int compare(const NodeID& a, const NodeID& b);
    
    /**
     * @brief 判断 a 是否比 b 有更高的优先级（Node ID 更大）
     */
    static bool hasPriority(const NodeID& a, const NodeID& b) {
        return compare(a, b) > 0;
    }
    
    /**
     * @brief Node ID 转十六进制字符串（用于调试）
     * @param nodeId Node ID
     * @param full 是否显示完整的 32 字节，默认只显示前 8 字节
     * @return 十六进制字符串
     */
    static std::string toString(const NodeID& nodeId, bool full = false);
    
    /**
     * @brief 检查 Node ID 是否为空（全零）
     */
    static bool isEmpty(const NodeID& nodeId);
};

#endif // NODE_IDENTITY_H
