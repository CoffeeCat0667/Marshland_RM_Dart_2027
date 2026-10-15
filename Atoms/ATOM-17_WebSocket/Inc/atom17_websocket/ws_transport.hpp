// ============================================================================
// ws_transport.hpp — ATOM-17 传输适配接口
//
// 与具体 WebSocket 库解耦的传输边界：公共头不暴露任何第三方库类型
// （civetweb 仅出现在 Src/civetweb_transport.cpp）。上层测试可注入自实现
// 的 IWsTransport 替身，不依赖真实网络即可驱动服务器核心。
//
// 线程约定：
//   - IHandler 回调由传输实现的工作线程发起，可能并发；实现方（服务器核心）
//     必须保证回调线程安全。
//   - 同一 session 的回调顺序保证：Open 先于 Message，Close 最后；
//     Close 之后不再有该 session 的任何回调。
//   - SendText/CloseSession 可由任意线程调用（服务器推送线程使用）。
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>

namespace marshland::atom17 {

class IWsTransport {
public:
    class IHandler {
    public:
        virtual ~IHandler() = default;
        virtual void OnSessionOpen(std::uint64_t session_id) noexcept = 0;
        virtual void OnSessionMessage(std::uint64_t session_id, const char* data,
                                      std::uint64_t size) noexcept = 0;
        virtual void OnSessionClose(std::uint64_t session_id) noexcept = 0;
    };

    virtual ~IWsTransport() = default;

    // 开始监听。io_timeout_ms 为单次读写/消息处理上限（设计值，实现方可用于
    // 传输层超时配置）。返回 false 表示监听失败。重复 Start 行为由实现定义，
    // 服务器核心保证只在停止状态下调用一次。
    virtual bool Start(const char* listen_address, std::int64_t port,
                       std::int64_t io_timeout_ms, IHandler& handler) noexcept = 0;

    // 停止监听并关闭全部连接；返回后不再发起任何 IHandler 回调。幂等。
    virtual void Stop() noexcept = 0;

    // 向指定会话发送一个文本帧；全部字节写入返回 true。
    // 会话不存在或连接已关闭返回 false（调用方据此清理会话）。
    virtual bool SendText(std::uint64_t session_id, const char* data,
                          std::uint64_t size) noexcept = 0;

    // 主动关闭指定会话（发送关闭帧）。会话随后经 OnSessionClose 收尾；
    // 会话不存在时为空操作。
    virtual void CloseSession(std::uint64_t session_id) noexcept = 0;
};

// 创建基于 CivetWeb 的传输实现（定义于 Src/civetweb_transport.cpp）。
std::unique_ptr<IWsTransport> CreateCivetwebTransport();

} // namespace marshland::atom17
