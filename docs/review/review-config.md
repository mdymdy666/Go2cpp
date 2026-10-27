# config 模块审查报告

- 审查对象：`src/config/config.cpp`（1325 行）、`src/config/so_manifest.cpp`（669 行）、`src/config/so_reloader.cpp`（507 行）、`include/go2cpp/config/{config,so_manifest,so_reloader}.hpp`
- 基线：`a2fa0df`；本模块文件 md5 与审查快照一致（同期 Bento 工作区有并发改动，不影响本模块行号）
- 审查方式：只读静态审查，未运行编译/测试
- 统计：**P0×1 / P1×6 / P2×5，共 12 条**

## 1. 问题清单

| ID | 级别 | 位置 | 问题 | 证据 | 建议修复 |
|---|---|---|---|---|---|
| CONF-01 | P0 | `src/config/config.cpp:657` | 文件事务锁在通知阶段前释放，两个并发 `LoadFromIni` 破坏「整批一次发布」的原子性 | `transaction_lock.unlock();`（657）之后才 `NotifyPending()`（662）；T2 可在 T1 通知前取锁（607）→`ApplyString`（645）→unlock（657）；T1 监听器读到的已是 T2 已写未发布值；若 T2 后续失败回滚，T1 监听器已观察「从未发布的值」且收不到回退通知。`config.hpp:511-513` 明确承诺「要么全部写入并广播，要么全部回滚」 | 把「提交+通知」放进同一把锁（recursive_mutex 允许监听器重入提交），或引入事务代次令牌让监听器只观察已发布代次；至少把 pending 冲突（`config.hpp:398`）定义为可重试错误并在 `LoadFromFile` 层退避重试 |
| CONF-02 | P1 | `src/config/so_reloader.cpp:232` | 失败/被拒事务也会永久抬高 `m_highest_seen`，且无回退/重置接口；一次坏 manifest 会让后续低 revision 发布全部被拒 | 232-237 在 `Validate`（265）与 loader 之前 CAS 抬高；243/257 的栅栏只比较、不回退；`ForceRescan()`（203-213）用 `m_current.revision` 也走 257 被拒。例：revision=999 校验失败后 committed=5，修复为 revision=6 会在 257 被「已有更高 revision 进入」拒绝 | CAS 抬高移到静态校验通过之后；或失败路径在无更高并发抢先时回退（带版本号的「已验证最高 revision」）；至少提供 admin 重置接口，并让 force_rescan 忽略 highest_seen 栅栏但保留 committed 回退检查 |
| CONF-03 | P1 | `src/config/so_reloader.cpp:342` | `ReloadImpl` 的已加载句柄与 staged pending 没有 RAII 守卫，异常逸出会泄漏 SO 句柄并永久毒化 ConfigVar | `loaded`（342）/`staged`（372）只在显式 `rollback()`（373-376）回收；`NowSeconds()`（377）、`StageString`→`m_config.Lookup`（171）的 `make_shared` 抛 bad_alloc 即栈展开；已设置的 `m_pending` 不清除，之后该键所有 `ApplyString/SetValue` 返回 false（`config.hpp:398-401、306-309`） | 加 scope guard（析构时未 commit 则 `RollbackStaged`+`UnloadLoaded`），成功路径显式 commit() 解除 |
| CONF-04 | P1 | `src/config/config.cpp:850` | 监听线程内停止时 detach，`Config` 析构后线程仍访问已释放成员，存在 UAF | `if (m_watcher.get_id() == std::this_thread::get_id()) m_watcher.detach();`（850）；lambda 捕获 `this`（789），之后仍循环读 `m_watching`（794/802）、锁 `m_watcher_mutex`（796）、读 `m_watcher_cv`（798）；`~Config`（578）→`StopWatcher`→`joinable()==false` 不等待。`config.hpp:514` 只写「除非在自身线程内停止」 | 监听线程状态收进 `shared_ptr<WatcherState>`，lambda 捕获 shared_ptr 而非 this；自线程停止只置停止位、join 交给下一次外部 StopWatcher；文档禁止回调内 StartWatcher（需人工确认现有用法） |
| CONF-05 | P1 | `src/config/so_reloader.cpp:417` | `m_reload_mutex` 包住 `NotifyPending` 与 SoActivator，回调重入 `Reload/ForceRescan` 会在同一线程重复加非递归 mutex | `std::lock_guard<std::mutex> reload_lock(m_reload_mutex);`（240）作用域到函数结束，417-419 `NotifyPending`、420-431 `m_activator` 都在其中；`Reload`（195）/`ForceRescan`（199）入口再取同一锁；`config.hpp:513` 对 Config 明确要求「事务锁绝不包住监听器回调」 | 提交段（NotifyPending+activator+m_current 更新）移出 `m_reload_mutex`，用独立 `m_commit_mutex`；或在接口契约禁止重入并加线程 ID 断言（需人工确认现有 activator 是否重入） |
| CONF-06 | P1 | `src/config/config.cpp:932` | `BindRuntimeConfig` 的跨字段校验器用裸指针读 `target`，写回监听器无锁写同一字段，构成数据竞争与 TOCTOU | validator 读 `target->scheduler.max_workers/min_workers`（938/947），监听器写 `*field = value`（953）；validator 在配置加载线程（持 `m_transaction_mutex`），监听器在任意提交线程的锁外执行（`config.hpp:316-325`），`RuntimeConfig` 是普通结构体。两个字段分别 SetValue 时各自读到旧值可同时通过，留下 min>max。对照 timer 绑定（1128-1139）用 weak_ptr+GetValue | 绑定字段改 `std::atomic<size_t>`，或维护带锁镜像；跨字段校验统一改用 `ConfigVar::GetValue`（弱引用），不要读裸指针 target |
| CONF-07 | P1 | `src/config/config.cpp:535` | `RuntimeConfig::Validate` 漏掉 `min_workers` 上限检查，与 `ValidateSchedulerSize` 规则漂移，可把非法值作为默认值发布 | 535-545 只检查 `max_workers/processor_count > kMaxWorkers`，没有 `min_workers`；`ValidateSchedulerSize`（176-182）对 `min_workers` 拒绝 >32。`FromIni` 走 `ReadSize` 安全（489-495），但直接构造 `RuntimeConfig{min_workers=100}` 后 `Validate` 返回 true（`BindRuntimeConfig:922` 的预检只调 Validate） | 加 `if (value.min_workers > kMaxWorkers) return fail(...)`；错误文本由 `kMax*` 常量生成，避免硬编码「32/1048576/67108864」漏改 |
| CONF-08 | P2 | `src/config/config.cpp:440` | section 行不支持行内注释，`[scheduler] ; 调度器` 会让整个文件解析失败 | `if (clean.front() == '[') { if (clean.back() != ']') { ... "节标题缺少 ]" ... } }`（440-445）；值行支持 #/; 截断，section 行不支持，`config.hpp:79-81` 也未说明 | section 行先按第一个未转义 #/; 截断再校验 `]`，或在错误信息中明示不支持行内注释 |
| CONF-09 | P2 | `src/config/config.cpp:110` | `ExtractIniValue` 只有 `\xHH` 是转义，与文档「值在第一个未转义的 #/; 处截断」矛盾；手写 `\\n` 被误还原为「反斜杠+换行」 | 113-116 只跳过 `IsHexEscapeAt`，`\#` 不被保护；125-129 的 `value.find("\\n")` 对字面量 `\\n`（两反斜杠+n）会匹配第二字符并重写成 `\`+换行。DumpIni 因把 `\` 编码成 `\x5c` 不受影响，但手写 `C:\\new` 会变成 `C:\`+换行 | 二选一：文档改为「只有 \xHH 是转义序列」；或实现 `\\`→`\`、`\#`→`#` 的成对转义并同步修改 DumpIni |
| CONF-10 | P2 | `src/config/config.cpp:619` | 无点变量与 `global.*` 变量映射到同一 INI 键，DumpIni 产生重复键、读回串值 | LoadFromIni 用 `section = ... ? "global" : ...`（619），DumpIni 把空 module 写成 `[global]`（1275）；`Lookup("foo")` 与 `Lookup("global.foo")` 都是 `[global] foo`，Parse 后者覆盖前者（454） | 为无点变量保留不会与模块名冲突的 section（如 `[.root]`），或禁止注册以 `global.` 开头的 name |
| CONF-11 | P2 | `src/config/config.cpp:1283` | DumpIni 只转义值不转义 key，Lookup 不校验 name 字符集，可生成不可解析的 INI | `out += key + "=" + EscapeIniValue(var->ToString()) + "\n";`（1283），key 来自 `SplitConfigName`（1280）；`Config::Lookup`（`config.hpp:541`）对 name 无校验；`Lookup("a=b")`/`Lookup("a\n[x]")` 会切错 key 或造出伪 section | Lookup/AutoConfigRegistrar 入口校验 name 只含 `[A-Za-z0-9_.-]` 且无空段，或对 key 做 \xHH 编码并在 Parse 端对称解码 |
| CONF-12 | P2 | `src/config/so_manifest.cpp:370` | `Validate` 无条件 clear 调用方诊断列表，与头文件「逐条追加」契约矛盾；错误文本还写着 RTLD_GLOBAL，与 RTLD_LOCAL 设计冲突 | `diags.clear();`（370）覆盖 `so_manifest.hpp:162-164`「在 diagnostics 非空时逐条追加」；479-481 报「（RTLD_GLOBAL 会互相覆盖）」，而 `so_manifest.hpp:69` 的设计是 RTLD_LOCAL + 显式 dlsym | 去掉 clear 改为追加（由调用方清空），或文档改为「覆盖写入」；错误文本改为「符号前缀须全局唯一（RTLD_LOCAL+dlsym 下重复前缀导致解析歧义）」（需人工确认 loader 实际 dlopen flags） |

## 2. 重点问题说明

### CONF-01（P0）事务原子性
`LoadFromIni` 的注释（590-601）明确承诺两阶段提交，但 `unlock`（657）发生在 `NotifyPending`（662）之前。并发窗口下，T1 的监听器会看到 T2 尚未发布的 pending 值；如果 T2 之后失败回滚，监听器已经按「已发布」的语义消费了这些值。这不是简单的锁粒度问题，而是破坏了「整批配置要么全旧、要么全新」的核心不变量，可能让 scheduler/log 等模块在回调里构建出互相矛盾的配置快照。

### CONF-04（P1）detach UAF
`stop_watcher_locked` 为避免自 join 死锁选择 `detach`，但 lambda 捕获的是 `this`，析构后线程仍会访问 `m_watching/m_watcher_mutex/m_watcher_cv`。`config.hpp:514` 只用一句「除非在自身线程内停止」带过，没有声明生命周期后果。建议把 watcher 状态做成 shared_ptr 独立对象。

### CONF-06（P1）跨字段校验的数据竞争
`SetValidator` 的注释声称校验器不会在配置锁内执行，但 `LoadFromIni:623` 正是在事务锁内调用 `ValidateString`；同时 validator 读 `target` 裸指针、listener 无锁写同一字段。两者叠加使 min/max 的跨字段校验既可能读到撕裂值，也可能出现 TOCTOU 放行。

## 3. 亮点

- 两阶段事务（预检 → 候选快照 → 跨字段校验 → ApplyString → 统一通知）设计方向正确，`ApplyString` 失败时逆序 `RollbackPending`，比多数配置库严谨。
- INI 的 `\xHH` 转义是自洽的（DumpIni 与 Parse 对称），`Parse` 先建临时表再 swap，避免半解析状态污染。
- `ReadStableFileContents` 用双快照 + 双锁检查抵御编辑器「截断再写」的读竞态，细节考虑到位。
- 头文件把状态机、不变量、错误语义写得清楚，是本次五个模块中接口文档质量最高的一份（尽管存在个别漂移，如 CONF-06 的校验器锁语义）。

## 4. 维度结论

| 维度 | 结论 |
|---|---|
| 正确性 | 事务语义在并发下被破坏（P0）；范围校验三处漂移（CONF-07）；INI 边界（section 行内注释、转义、key 转义）有洞；`ParseSize/ParseU64` 用 `from_chars` 严格，但 `ValueCodec<T>` 走 `istringstream`，无符号类型的负数行为需人工确认 |
| 并发 | `GetValue` 原子 shared_ptr 快照、写路径串行，方向正确；问题集中在锁边界：通知前 unlock、SoReloader 锁内跑回调、BindRuntimeConfig 裸指针、watcher detach |
| 资源与错误 | 文件流 RAII、SaveToFile 有 tmp+rename；缺文件大小/行长度上限（303-309、435-436），外部输入可撑爆内存；SoReloader 异常安全缺口见 CONF-03 |
| 风格与可维护性 | 头文件状态机/不变量文档质量高；`BindRuntimeConfig` 的 7 个 size+4 个 duration+3 个 bool 绑定高度重复；上限值同时硬编码在 `kMax*` 与错误字符串；`AddTransactionValidator` 返回 id 被丢弃（1105），多次绑定会累积无法移除的校验器 |

## 5. 未覆盖与限制

- 未执行编译/测试；`ValueCodec<size_t>::Decode("-1")` 在 libstdc++ 的具体行为、SoReloader 实际 dlopen flags、StopWatcher 回调场景均标注「需人工确认」。
- `Config::Remove`（1240）与 `LoadFromIni` 的 List 快照（609）竞态、`ValueCodec<vector<string>>` 的逗号不对称、`IniFile::Parse` 重复键静默覆盖属于低信号观察，未纳入 12 条。
