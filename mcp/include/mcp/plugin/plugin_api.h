/**
 * @file plugin_api.h
 * @brief 与参考项目 mcp-extened 兼容的插件 C ABI。
 *
 * @details
 * 结构体布局与 `mcp-extened/mcp_server/src/interface/PluginAPI.h`（MIT，
 * Copyright (C) 2025 Giuseppe Mastrangelo）保持一致，目的是**直接复用已有的
 * .so 插件二进制**（参考项目 build 目录下的 libs），而不是重新发明一套 ABI。
 * 字段顺序、回调签名一处都不能改，否则 dlopen 后调用会崩。
 *
 * 与上游的差异只在注释：逐字段说明用途，避免使用者按名字猜语义。
 */

#pragma once

#ifdef _WIN32
#define MCP_PLUGIN_API __declspec(dllexport)
#else
#define MCP_PLUGIN_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 插件向客户端发通知的回调（由宿主注入）。
 *
 * 语义（宿主侧实现见 `src/plugin/plugin_loader.cpp` 的 `NotificationHub`）：
 * - `notification` 是 UTF-8 JSON 字符串：可以是完整的 JSON-RPC 通知帧
 *   `{"jsonrpc":"2.0","method":"notifications/message","params":{...}}`，
 *   也可以只给 params 对象（宿主按 `notifications/message` 转发）；
 * - method 必须以 `notifications/` 开头，否则宿主拒绝（插件不能伪造请求/响应）；
 * - **线程安全**：可在插件的工作线程调用，宿主内部串行化写传输；
 * - 无客户端连接或插件已卸载时宿主**丢弃**通知（不缓存），不会回调到已销毁对象；
 * - 回调不得抛出 C++ 异常（跨 .so 边界），宿主实现也不向插件抛异常。
 */
typedef void (*ClientNotificationCallback)(const char* pluginName, const char* notification);

/// 插件类别，决定它出现在 tools / prompts / resources 哪个列表。
typedef enum {
    PLUGIN_TYPE_TOOLS = 0,     ///< 工具插件
    PLUGIN_TYPE_PROMPTS = 1,   ///< 提示插件
    PLUGIN_TYPE_RESOURCES = 2, ///< 资源插件
} PluginType;

/// 工具描述（宿主只读，指针归插件所有）。
typedef struct {
    const char* name;         ///< 工具名
    const char* description;  ///< 人类可读描述
    const char* inputSchema;  ///< JSON Schema 字符串
} PluginTool;

/// 提示描述。
typedef struct {
    const char* name;         ///< 提示名
    const char* description;  ///< 描述
    const char* arguments;    ///< 参数 schema（JSON 字符串）
} PluginPrompt;

/// 资源描述。
typedef struct {
    const char* name;         ///< 资源名
    const char* description;  ///< 描述
    const char* uri;          ///< 资源 URI
    const char* mime;         ///< MIME 类型
} PluginResource;

/**
 * @brief 通知通道（宿主填充，插件调用）。
 *
 * 结构体由**宿主分配并持有**（每个插件加载时一份，地址在插件卸载前保持稳定），
 * 插件只能通过 `PluginAPI::notifications` 读取后调用，不得 `delete` 或替换。
 */
typedef struct {
    ClientNotificationCallback SendToClient;  ///< 插件调它把通知推给客户端
} NotificationSystem;

/**
 * @brief 插件导出表。
 *
 * 约定：`HandleRequest` 收到完整 JSON-RPC 请求字符串，返回**堆分配**的 JSON
 * 字符串；宿主负责 `delete[]`。`GetTool/GetPrompt/GetResource` 在 index 越界
 * 时必须返回 nullptr 或由宿主保证不越界调用。
 */
typedef struct {
    const char* (*GetName)();
    const char* (*GetVersion)();
    PluginType (*GetType)();
    int (*Initialize)();                              ///< 返回 1 表示成功
    char* (*HandleRequest)(const char* request);      ///< 返回 new[] 分配的字符串
    void (*Shutdown)();
    int (*GetToolCount)();
    const PluginTool* (*GetTool)(int index);
    int (*GetPromptCount)();
    const PluginPrompt* (*GetPrompt)(int index);
    int (*GetResourceCount)();
    const PluginResource* (*GetResource)(int index);
    NotificationSystem* notifications;                ///< 宿主在 Initialize 前注入；nullptr = 宿主不支持
} PluginAPI;

/// 插件导出的两个符号。
MCP_PLUGIN_API PluginAPI* CreatePlugin();
MCP_PLUGIN_API void DestroyPlugin(PluginAPI*);

#ifdef __cplusplus
}
#endif
