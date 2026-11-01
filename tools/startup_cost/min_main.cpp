/**
 * @file min_main.cpp
 * @brief 场景 A 的基线程序：只有 `main` 与 `return`。
 *
 * 它不链接项目任何库，量出的是「一个动态链接的 C++ 程序在本机从 exec
 * 到退出」的底噪。基准报告的每个池增量都应该和它比，而不是和 0 比。
 *
 * 编译产物：`.build-startup-cost/startup_cost_min`
 */
int main() { return 0; }
