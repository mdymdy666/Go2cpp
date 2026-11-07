/**
 * @file atomic_file.cpp
 * @brief `atomic_file.hpp` 的平台实现。
 */
#include "go2cpp/platform/atomic_file.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#endif

namespace go2cpp::platform {

#if defined(_WIN32)
namespace {

/// @brief UTF-8 → UTF-16（Windows API 只用宽字符）。
std::wstring ToWide(const std::string& s) {
  if (s.empty()) return {};
  const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                         static_cast<int>(s.size()), nullptr, 0);
  if (need <= 0) return {};
  std::wstring out(static_cast<std::size_t>(need), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), need);
  return out;
}

}  // namespace
#endif

bool AtomicRename(const std::string& from, const std::string& to, int* error) {
#if defined(_WIN32)
  const std::wstring wfrom = ToWide(from);
  const std::wstring wto = ToWide(to);
  if (wfrom.empty() || wto.empty()) {
    if (error != nullptr) *error = ERROR_INVALID_PARAMETER;
    return false;
  }
  // MOVEFILE_REPLACE_EXISTING：目标存在时覆盖（对应 POSIX rename 语义）
  if (::MoveFileExW(wfrom.c_str(), wto.c_str(), MOVEFILE_REPLACE_EXISTING) == 0) {
    if (error != nullptr) *error = static_cast<int>(::GetLastError());
    return false;
  }
  return true;
#else
  if (::rename(from.c_str(), to.c_str()) != 0) {
    if (error != nullptr) *error = errno;
    return false;
  }
  return true;
#endif
}

bool DeleteFileIfExists(const std::string& path, int* error) {
#if defined(_WIN32)
  const std::wstring wpath = ToWide(path);
  if (wpath.empty()) {
    if (error != nullptr) *error = ERROR_INVALID_PARAMETER;
    return false;
  }
  if (::DeleteFileW(wpath.c_str()) == 0) {
    const DWORD err = ::GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
      return true;  // 幂等
    }
    if (error != nullptr) *error = static_cast<int>(err);
    return false;
  }
  return true;
#else
  if (::remove(path.c_str()) != 0) {
    if (errno == ENOENT) {
      return true;  // 幂等
    }
    if (error != nullptr) *error = errno;
    return false;
  }
  return true;
#endif
}

}  // namespace go2cpp::platform
