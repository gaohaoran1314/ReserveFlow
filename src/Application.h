// =============================================================================
// src/app/Application.h  —  装配根
// 对应 ARCH-P0 §3.13 / §1.1
// 职责：配置 → 日志 → DbPool → Redis → Jwt → Repo → Service → Controller → 路由 → listen
// 约束：不写任何业务判断；pImpl 隐藏全部实现类型，避免头文件拖入 httplib/MySQL。
// =============================================================================
#pragma once

#include <memory>
#include <string>

namespace rf {

/** 应用装配与生命周期 */
class Application {
public:
    Application();
    ~Application();
    Application(const Application&)            = delete;
    Application& operator=(const Application&) = delete;

    /**
     * 装配全部依赖并注册路由。
     * @param configPath config.json 路径
     * @param err        失败时写入可读原因（配置缺失/非法 → 调用方回报 1011）
     * @return 失败返回 false
     */
    bool init(const std::string& configPath, std::string& err);

    /** 阻塞监听；Ctrl+C / stop() 后返回进程退出码（0 表示正常结束） */
    int run();

    /** 主动停止 HTTP 服务（可从信号处理调用） */
    void stop();

private:
    void registerRoutes();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rf
