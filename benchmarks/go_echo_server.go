// go_echo_server.go —— 与 gmp_vs_epoll_fair_benchmark 同协议、同业务的 Go 对照 server。
//
// 公平性约束（与 C++ 侧逐项对齐）：
//   - 同一帧协议：[u32 LE length][u8 opcode][body]，length = 1 + len(body)；
//   - 同一业务函数 process_body：xorshift seed 递推，count = seed % 4097，
//     生成 count 个 int32 并排序；opcode 1=echo/2=read/3=write/5=ping；
//   - 同一读取方式：64KB 块读入（与 C++ GMP 的 char buffer[64*1024] 对齐）、
//     累积缓冲按需增长（与 std::string in 对齐）、按帧解析；
//   - 同一写出方式：每响应聚合 header + body 后 1 次 Write，out 按需增长；
//   - C++ 侧未设 TCP_NODELAY（Linux 默认 Nagle 开启），这里显式 SetNoDelay(false) 对齐；
//   - GOMAXPROCS=workers，主 goroutine accept，每连接一个 goroutine 内联处理。
package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"math"
	"net"
	"os"
	"runtime"
	"slices"
)

const (
	kMaxFrame = 8 << 20
	kBufSize  = 64 * 1024
)

type connState struct {
	seed    uint32
	scratch []int32
}

// process_body 与 C++ 的 process_body 逐行等价（包括返回新分配的 echo body）。
func processBody(opcode byte, body []byte, st *connState) []byte {
	st.seed ^= st.seed << 13
	st.seed ^= st.seed >> 17
	st.seed ^= st.seed << 5
	count := int(st.seed % 4097)
	if count > 1 {
		if cap(st.scratch) < count {
			st.scratch = make([]int32, count)
		} else {
			st.scratch = st.scratch[:count]
		}
		r := st.seed | 1
		for i := 0; i < count; i++ {
			r ^= r << 13
			r ^= r >> 17
			r ^= r << 5
			st.scratch[i] = int32(r)
		}
		slices.Sort(st.scratch)
		if st.scratch[0] == math.MinInt32 {
			return nil
		}
	}
	switch opcode {
	case 1:
		out := make([]byte, len(body))
		copy(out, body)
		return out
	case 2:
		return []byte{'A'}
	case 3:
		out := make([]byte, 4096)
		for i := range out {
			out[i] = 'W'
		}
		return out
	case 5:
		return []byte{'P'}
	default:
		return nil
	}
}

func handle(conn *net.TCPConn) {
	defer conn.Close()
	_ = conn.SetNoDelay(false) // 对齐 C++ 侧：未设置 TCP_NODELAY（Nagle 开启）。

	st := &connState{seed: 0x9e3779b9}
	buf := make([]byte, kBufSize) // 与 C++ GMP 的每连接 64KB 读缓冲对齐。
	in := make([]byte, 0, 4096)   // 与 std::string 对齐：按需增长，不预分配 64KB。
	inOff := 0
	out := make([]byte, 0, 256) // 小响应为主，按需增长。

	for {
		for len(in)-inOff >= 4 {
			length := int(binary.LittleEndian.Uint32(in[inOff:]))
			if length == 0 || length > kMaxFrame {
				return
			}
			if len(in)-inOff < 4+length {
				break
			}
			body := in[inOff+4 : inOff+4+length]
			resp := processBody(body[0], body[1:], st)
			out = out[:0]
			out = binary.LittleEndian.AppendUint32(out, uint32(len(resp)))
			out = append(out, resp...)
			if _, err := conn.Write(out); err != nil {
				return
			}
			inOff += 4 + length
		}
		if inOff == len(in) {
			in = in[:0]
			inOff = 0
		} else if inOff > 1<<20 {
			in = append(in[:0], in[inOff:]...)
			inOff = 0
		}
		n, err := conn.Read(buf)
		if n > 0 {
			in = append(in, buf[:n]...)
			continue
		}
		if err != nil {
			return
		}
		return
	}
}

func main() {
	port := flag.Int("port", 0, "listen port (0 = ephemeral)")
	workers := flag.Int("workers", 12, "GOMAXPROCS")
	flag.Parse()

	runtime.GOMAXPROCS(*workers)

	ln, err := net.Listen("tcp4", fmt.Sprintf("127.0.0.1:%d", *port))
	if err != nil {
		fmt.Fprintf(os.Stderr, "listen failed: %v\n", err)
		os.Exit(1)
	}
	actual := ln.Addr().(*net.TCPAddr).Port
	fmt.Printf("READY pid=%d port=%d\n", os.Getpid(), actual)
	os.Stdout.Sync()

	for {
		conn, err := ln.Accept()
		if err != nil {
			continue
		}
		go handle(conn.(*net.TCPConn))
	}
}
