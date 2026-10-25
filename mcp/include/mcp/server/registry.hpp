/**
 * @file registry.hpp
 * @brief 工具 / 提示 / 资源的注册表。
 *
 * @details
 * 参考项目用动态库插件（`dlopen` + C ABI）承载这些内容，注册表只是插件列表。
 * 这里拆成两步：**注册表负责「有什么、怎么调」**，插件加载以后可以叠在上面
 * （`RegisterTool` 在加载后调用即可），核心协议逻辑不再被动态库绑定。
 *
 * 线程安全：所有公开方法内部加锁，允许在服务运行期间动态注册/替换。
 * 回调在锁内拷贝一份 `std::function` 后释放锁再执行，避免用户回调里再次
 * 注册导致自死锁。
 *
 * 变更事件：注册/覆盖/注销都会通知 `AddToolListener` 注册的监听器。
 * 监听器同样在注册表锁**外**调用（先做快照），因此监听器内部可以安全地
 * 再次注册/注销工具；检索层靠这条通路做增量索引，不需要全量 `Refresh()`。
 */

#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "mcp/protocol/types.hpp"

namespace go2cpp::mcp {

/// 工具变更事件类型（检索层据此增量维护索引）。
enum class ToolChangeKind {
    kAdded,    ///< 新注册：此前没有同名工具
    kUpdated,  ///< 同名覆盖：名字不变，描述/schema/handler 可能变了
    kRemoved   ///< 注销：工具已从注册表移除
};

/**
 * @brief 工具变更监听器。
 * @param kind 事件类型
 * @param tool 变更后的工具定义；`kRemoved` 时是被移除前的最后一份定义
 */
using ToolChangeListener = std::function<void(ToolChangeKind kind, const Tool& tool)>;

/**
 * @brief 工具、提示、资源的容器。
 *
 * 同一名字重复注册时**覆盖**旧项（便于热更新），而不是报错或追加。
 */
class Registry {
public:
    Registry() = default;
    /// 移动构造/赋值：内部有 mutex，编译器不会自动生成；构造 Server 时用得上。
    Registry(Registry&& other) noexcept;
    Registry& operator=(Registry&& other) noexcept;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    void RegisterTool(Tool tool);
    /// 注销工具；返回是否真的删掉了（不存在返回 false）。
    bool UnregisterTool(const std::string& name);
    void RegisterPrompt(Prompt prompt);
    void RegisterResource(Resource resource);

    /**
     * @brief 订阅工具变更事件。
     * @details 监听器在注册表锁**外**调用，内部可以安全地再次注册/注销工具；
     *          通知使用调用开始时的监听器快照，回调中增删监听器不影响本次通知。
     * @return 监听器 id，用于 `RemoveToolListener`。
     */
    std::size_t AddToolListener(ToolChangeListener listener);
    /// 退订监听器；返回该 id 是否存在。
    bool RemoveToolListener(std::size_t id);
    /// 当前监听器数量（诊断/测试用）。
    std::size_t tool_listener_count() const;

    /// `tools/list` 的 result 对象：`{"tools":[...]}`。
    json ToolList() const;
    /// `prompts/list` 的 result 对象：`{"prompts":[...]}`。
    json PromptList() const;
    /// `resources/list` 的 result 对象：`{"resources":[...]}`。
    json ResourceList() const;

    /**
     * @brief 调用工具。
     * @param context 工具上下文（参数、meta、通知通道）。
     * @return 工具不存在时返回 `std::nullopt`；否则返回工具结果（异常由调用方捕获）。
     */
    /// 取工具的 inputSchema；工具不存在返回 nullopt（校验层使用）。
    std::optional<json> ToolSchema(const std::string& name) const;

    std::optional<ToolResult> InvokeTool(const std::string& name, const ToolContext& context) const;

    /// 获取提示；不存在返回 `std::nullopt`。
    std::optional<json> GetPrompt(const std::string& name, const json& arguments) const;

    /// 读取资源；不存在返回 `std::nullopt`。
    std::optional<json> ReadResource(const std::string& uri) const;

    std::size_t tool_count() const;
    std::size_t prompt_count() const;
    std::size_t resource_count() const;

private:
    /// 锁内拷贝监听器，供锁外通知使用（避免持锁回调导致自死锁）。
    std::vector<ToolChangeListener> ListenerSnapshotLocked() const;

    mutable std::mutex m_mutex;
    std::vector<Tool> m_tools;
    std::vector<Prompt> m_prompts;
    std::vector<Resource> m_resources;
    std::unordered_map<std::size_t, ToolChangeListener> m_tool_listeners;
    std::size_t m_next_listener_id = 1;  ///< 监听器 id 分配器（单调递增）
};

}  // namespace go2cpp::mcp
