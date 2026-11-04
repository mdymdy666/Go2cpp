// Command ws_bench_go 是 Go 标准库实现的 WebSocket echo 服务端，
// 作为 Go2Cpp WebSocket 网关的对照基线。
//
// 只依赖标准库（环境无外网，不能用 gorilla/websocket）：
//   - net/http 负责 TCP 监听与 HTTP 解析；
//   - Hijack 后手写 101 响应，Sec-WebSocket-Accept = base64(sha1(key+GUID))；
//   - 最小帧编解码：解析客户端掩码帧，Text/Binary 原样回显，Ping 回 Pong，
//     Close 回 Close 并退出。
//
// 用法：
//
//	./ws_bench_go --port 0 --workers 4 --max-conn 0
//
// 启动后打印 READY pid=<pid> port=<port>，收到 SIGTERM/SIGINT 后优雅退出。
package main

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"runtime"
	"sync/atomic"
	"syscall"
	"time"
)

const websocketGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

var (
	activeConns  atomic.Int64
	totalConns   atomic.Int64
	totalEchoed  atomic.Int64
	rejectedConn atomic.Int64
	maxConn      int64
)

// acceptKey 按 RFC 6455 §4.2.2 计算 Sec-WebSocket-Accept。
func acceptKey(key string) string {
	sum := sha1.Sum([]byte(key + websocketGUID))
	return base64.StdEncoding.EncodeToString(sum[:])
}

// readFrame 从 r 读取一个完整的客户端帧（必须带掩码）。
// 返回 opcode 与已解掩码的 payload；对端关闭或 IO 错误返回 error。
func readFrame(r *bufio.Reader) (byte, []byte, error) {
	var header [2]byte
	if _, err := io.ReadFull(r, header[:]); err != nil {
		return 0, nil, err
	}
	fin := header[0]&0x80 != 0
	opcode := header[0] & 0x0f
	masked := header[1]&0x80 != 0
	if !masked {
		return 0, nil, errors.New("client frame must be masked")
	}
	length := uint64(header[1] & 0x7f)
	switch length {
	case 126:
		var extended [2]byte
		if _, err := io.ReadFull(r, extended[:]); err != nil {
			return 0, nil, err
		}
		length = uint64(binary.BigEndian.Uint16(extended[:]))
	case 127:
		var extended [8]byte
		if _, err := io.ReadFull(r, extended[:]); err != nil {
			return 0, nil, err
		}
		length = binary.BigEndian.Uint64(extended[:])
	}
	if length > 16*1024*1024 {
		return 0, nil, errors.New("frame too large")
	}
	var mask [4]byte
	if _, err := io.ReadFull(r, mask[:]); err != nil {
		return 0, nil, err
	}
	payload := make([]byte, length)
	if _, err := io.ReadFull(r, payload); err != nil {
		return 0, nil, err
	}
	for i := range payload {
		payload[i] ^= mask[i%4]
	}
	_ = fin
	return opcode, payload, nil
}

// writeFrame 发送一个不带掩码的服务端帧。
func writeFrame(w io.Writer, opcode byte, payload []byte) error {
	header := make([]byte, 0, 10+len(payload))
	header = append(header, 0x80|opcode)
	switch {
	case len(payload) < 126:
		header = append(header, byte(len(payload)))
	case len(payload) <= 0xffff:
		header = append(header, 126, byte(len(payload)>>8), byte(len(payload)))
	default:
		header = append(header, 127)
		var extended [8]byte
		binary.BigEndian.PutUint64(extended[:], uint64(len(payload)))
		header = append(header, extended[:]...)
	}
	header = append(header, payload...)
	_, err := w.Write(header)
	return err
}

// serveConn 在 Hijack 后的连接上完成 101 握手并进入帧回显循环。
func serveConn(conn net.Conn, brw *bufio.ReadWriter, request *http.Request) {
	key := request.Header.Get("Sec-WebSocket-Key")
	if key == "" {
		_, _ = conn.Write([]byte("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n"))
		_ = conn.Close()
		return
	}
	response := "HTTP/1.1 101 Switching Protocols\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Accept: " + acceptKey(key) + "\r\n\r\n"
	if _, err := conn.Write([]byte(response)); err != nil {
		_ = conn.Close()
		return
	}

	index := activeConns.Add(1)
	if maxConn > 0 && index > maxConn {
		rejectedConn.Add(1)
		_ = writeFrame(conn, 0x8, []byte{0x03, 0xf5}) // 1013 server busy
		_ = conn.Close()
		return
	}
	defer func() {
		activeConns.Add(-1)
		_ = conn.Close()
	}()

	reader := brw.Reader
	for {
		opcode, payload, err := readFrame(reader)
		if err != nil {
			return
		}
		switch opcode {
		case 0x1, 0x2: // Text / Binary：原样回显
			totalEchoed.Add(1)
			if err := writeFrame(conn, opcode, payload); err != nil {
				return
			}
		case 0x9: // Ping → Pong，payload 原样带回
			if err := writeFrame(conn, 0xA, payload); err != nil {
				return
			}
		case 0xA: // Pong：客户端心跳的应答，无需处理
		case 0x8: // Close：回 Close 后退出
			_ = writeFrame(conn, 0x8, payload)
			return
		default:
			return
		}
	}
}

func main() {
	port := flag.Int("port", 0, "listen port (0 = kernel assigned)")
	workers := flag.Int("workers", 4, "GOMAXPROCS equivalent worker count")
	limit := flag.Int64("max-conn", 0, "max concurrent connections (0 = unlimited)")
	flag.Parse()

	maxConn = *limit
	runtime.GOMAXPROCS(*workers)

	mux := http.NewServeMux()
	mux.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Upgrade") != "websocket" {
			http.Error(w, "expected websocket upgrade", http.StatusBadRequest)
			return
		}
		hijacker, ok := w.(http.Hijacker)
		if !ok {
			http.Error(w, "hijack unsupported", http.StatusInternalServerError)
			return
		}
		conn, brw, err := hijacker.Hijack()
		if err != nil {
			return
		}
		totalConns.Add(1)
		serveConn(conn, brw, r)
	})

	listener, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", *port))
	if err != nil {
		log.Fatalf("listen failed: %v", err)
	}
	server := &http.Server{Handler: mux}

	fmt.Printf("READY pid=%d port=%d\n", os.Getpid(), listener.Addr().(*net.TCPAddr).Port)
	fmt.Printf("listening on ws://127.0.0.1:%d/ws\n", listener.Addr().(*net.TCPAddr).Port)
	os.Stdout.Sync()

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-stop
		_ = server.Close()
	}()

	if err := server.Serve(listener); err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Fatalf("serve failed: %v", err)
	}

	time.Sleep(20 * time.Millisecond)
	fmt.Printf("STATS handshakes=%d rejected=%d messages=%d active=%d\n",
		totalConns.Load(), rejectedConn.Load(), totalEchoed.Load(), activeConns.Load())
	os.Stdout.Sync()
}
