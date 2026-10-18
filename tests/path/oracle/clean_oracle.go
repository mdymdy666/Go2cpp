// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.
//
// 对拍 oracle：从 stdin 逐行读取路径，输出 Go `path.Clean` 的结果。
// Go2Cpp 的 tests/path/scripts/cross_check.py 用它生成/校验 1 万条对拍数据。
package main

import (
	"bufio"
	"fmt"
	"os"
	"path"
)

func main() {
	scanner := bufio.NewScanner(os.Stdin)
	// 随机语料含 300 字节左右的长段，默认 64 KiB 足够；放大到 4 MiB 以防万一。
	scanner.Buffer(make([]byte, 0, 64*1024), 4*1024*1024)
	out := bufio.NewWriter(os.Stdout)
	defer func() {
		if err := out.Flush(); err != nil {
			fmt.Fprintln(os.Stderr, "flush:", err)
			os.Exit(1)
		}
	}()
	for scanner.Scan() {
		if _, err := fmt.Fprintln(out, path.Clean(scanner.Text())); err != nil {
			fmt.Fprintln(os.Stderr, "write:", err)
			os.Exit(1)
		}
	}
	if err := scanner.Err(); err != nil {
		fmt.Fprintln(os.Stderr, "scan:", err)
		os.Exit(1)
	}
}
