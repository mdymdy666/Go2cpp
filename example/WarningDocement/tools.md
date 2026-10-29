# tools 使用限制

> 依据：`example/tools/` 示例 + `example/README.md`；每条标题末尾给出 file:line。

## 1. panic 不展开栈、不跳转 —— `example/tools/control_flow_full.cpp:16-18,110-117`
它是显式状态发布：`failure.call()` 之后的代码**继续执行**，无人 recover 也不终止进程。
```cpp
failure.call("业务失败", 503);
reached_after_call = true;   // 这一行会执行（与 Go 不同）
```
**后果**：照 Go 习惯写 → 以为不会继续跑，实际带错误继续。
## 2. `recover.take()` 只在 defer 回调内成功 —— `example/tools/control_flow_full.cpp:13,123-130`
边界外调用返回空，且不消费状态。
```cpp
defer cleanup([&] { if (auto r = recovery.take()) { /* 处理 */ } });
```
**后果**：在普通代码里 take 拿不到值，异常/错误被漏掉。
## 3. defer 回调抛异常被记录而不中断 —— `example/tools/control_flow_full.cpp:14`
其余 defer 仍会执行，异常不会传播给调用者。
```cpp
defer d([] { throw std::runtime_error("x"); });   // 只记录
```
**后果**：清理失败被静默吞掉，必须自己检查状态。
## 4. `run_now()` 不会让析构跳过 —— `example/tools/control_flow_full.cpp:87`
提前执行后 `active()` 仍可能为真，作用域结束会再跑一次。
```cpp
early.run_now();   // 要用 dismiss() 才能阻止析构时再执行
```
**后果**：同一清理动作执行两次。
## 5. 内建 Select case 要求可复制值 —— `example/README.md:183-185`
`SelectValue` 支持 move-only，但 Channel Select / EventBatch 的内建 case 要求可复制，否则返回 `kInvalid`。
```cpp
Select({RecvCase(move_only_chan)}, {}, 100ms);   // 返回 kInvalid，不等待
```
**后果**：以为会阻塞，实际立刻拿到无效结果。
## 6. 不能用 `Message()` 判断错误层级 —— `example/tools/error_full.cpp:62-64`
`Wrap` 产出的 Message 是「外层: 内层」完整链。
```cpp
const bool same = (outer != nullptr) && (level1 == middle);   // 用身份比较
```
**后果**：按文本匹配判断层级必错。
## 7. `errors::Is` 用身份比较 —— `example/tools/error_full.cpp:89-95`
同一个文本的新对象不会命中；`Is(nil, nil)` 才为真。
```cpp
Is(wrapped, errors::New("disk full"));   // false：不是同一对象
```
**后果**：用相同文案构造错误做判定 → 永远不匹配。
## 8. `MultiReader` 段间 EOF 必须清 —— `example/tools/streams_full.cpp:102`
逐字节读跨段时，段结束的 EOF 不能被当成整体结束。
```cpp
MultiReader drip({&first, &second});   // 段间 EOF 内部清掉
```
**后果**：只读到第一段就停。
## 9. `ReadFull` 的不完整尾帧与空源不同 —— `example/README.md:69`
不完整尾帧返回 `ErrUnexpectedEOF`，空源返回 `EOF`。
```cpp
if (err == ErrUnexpectedEOF) { /* 截断，不是正常结束 */ }
```
**后果**：把截断当正常 EOF → 静默丢数据。
## 10. Debug 计时不能当性能结论 —— `example/tools/strings_full.cpp:10` 等
各 full 示例的耗时只为确认「没有爆炸性劣化」，结论必须用 Release 基准（build-bench）。
```cpp
// Release: GO2CPP_BUILD_BENCHMARKS=ON 后跑 build-bench
```
**后果**：拿 Debug 数字做容量规划会偏差数倍。
