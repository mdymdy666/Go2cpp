# 把 BPF ELF 嵌入为 C++ 字节数组。
#
# 用法：
#   cmake -DBPF_OBJ=<x.bpf.o> -DBPF_OUT=<x_elf.cpp> [-DBPF_SYMBOL=<prefix>]
#         -P embed_bpf.cmake
#
# BPF_SYMBOL 是生成的符号前缀，缺省 go2cpp_net_stats（保持旧行为）：
#   <prefix>_elf     字节数组
#   <prefix>_elf_len 字节数
#
# 注意「先 extern 声明、再定义」不是冗余：C++ 命名空间作用域的 const 变量
# 默认是**内部链接**，只写 `extern "C" { const ... = ...; }` 时符号不会进入
# 静态库符号表，用户态 .cpp 链接期会报 undefined reference。
#
# 选择构建期嵌入而不是运行时读 .bpf.o：安装后的库不依赖数据文件路径，
# 也不会因为发布包漏拷一个 .o 而静默失去 eBPF 能力。

if(NOT DEFINED BPF_OBJ OR NOT DEFINED BPF_OUT)
    message(FATAL_ERROR "embed_bpf.cmake: BPF_OBJ and BPF_OUT are required")
endif()
if(NOT DEFINED BPF_SYMBOL OR BPF_SYMBOL STREQUAL "")
    set(BPF_SYMBOL "go2cpp_net_stats")
endif()

file(READ "${BPF_OBJ}" hex_content HEX)
string(LENGTH "${hex_content}" hex_length)
math(EXPR byte_length "${hex_length} / 2")

# "7f454c46" -> "0x7f,0x45,0x4c,0x46,"
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," byte_list "${hex_content}")

file(WRITE "${BPF_OUT}" "// 由 embed_bpf.cmake 生成，请勿手工编辑。\n")
file(APPEND "${BPF_OUT}" "extern \"C\" {\n")
file(APPEND "${BPF_OUT}" "extern const unsigned char ${BPF_SYMBOL}_elf[];\n")
file(APPEND "${BPF_OUT}"
     "const unsigned char ${BPF_SYMBOL}_elf[] = {${byte_list}};\n")
file(APPEND "${BPF_OUT}" "extern const unsigned long ${BPF_SYMBOL}_elf_len;\n")
file(APPEND "${BPF_OUT}"
     "const unsigned long ${BPF_SYMBOL}_elf_len = ${byte_length}UL;\n")
file(APPEND "${BPF_OUT}" "}\n")
