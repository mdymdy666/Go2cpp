/**
 * @file errors.cpp
 * @brief `compress` 模块哨兵错误的唯一定义。
 *
 * @details 每个哨兵都用自己的函数内静态指针（故意泄漏），保证全进程一份、
 *   指针身份比较可用。**注意不能抽成共用 helper**：helper 里的 static 会是
 *   同一个对象，八个错误就全串成同一个身份了（`bufio` 里踩过同样的坑，
 *   那里也保留了八份独立的 static）。
 */
#include "go2cpp/compress/errors.hpp"

namespace go2cpp::compress {

ErrorPtr ErrCorrupt() {
  static const auto* const kOwned =
      new ErrorPtr(NewError("flate: corrupt input"));
  return *kOwned;
}

ErrorPtr ErrChecksum() {
  static const auto* const kOwned =
      new ErrorPtr(NewError("compress: checksum mismatch"));
  return *kOwned;
}

ErrorPtr ErrHeader() {
  static const auto* const kOwned =
      new ErrorPtr(NewError("compress: invalid header"));
  return *kOwned;
}

ErrorPtr ErrUnexpectedEof() {
  static const auto* const kOwned =
      new ErrorPtr(NewError("compress: unexpected EOF"));
  return *kOwned;
}

ErrorPtr ErrInvalidLevel() {
  static const auto* const kOwned =
      new ErrorPtr(NewError("compress: invalid compression level"));
  return *kOwned;
}

ErrorPtr ErrClosed() {
  static const auto* const kOwned =
      new ErrorPtr(NewError("compress: writer is closed"));
  return *kOwned;
}

ErrorPtr ErrDictionary() {
  static const auto* const kOwned =
      new ErrorPtr(NewError("zlib: invalid dictionary"));
  return *kOwned;
}

ErrorPtr ErrInternal() {
  static const auto* const kOwned =
      new ErrorPtr(NewError("compress: internal zlib error"));
  return *kOwned;
}

}  // namespace go2cpp::compress
