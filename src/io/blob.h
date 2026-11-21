// 整块字节的文件读写：路径一律 UTF-8，内部转宽字符再走 Win32。
//
// libvips 的 foreign loader 在 Windows 上按 ANSI 解文件名，中文/日文路径会
// "file does not exist"；所以图片一律由这里读成内存再交给 libvips 的 buffer
// 入口，导出也先编码到内存再由这里落盘。文件名编码不再是任何一层的事。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace bl::io {

// 读整个文件；失败返回 false（文件不存在/无权限/读中断）。
bool ReadFileBytes(const std::string& utf8_path,
                   std::vector<unsigned char>* out);

// 覆盖写整个文件；失败返回 false。
bool WriteFileBytes(const std::string& utf8_path, const void* data,
                    std::size_t size);

// 从 exe 自己的资源里取 RCDATA（按 app.rc 里的名字）。找不到返回 false。
bool ReadResourceBytes(const wchar_t* name, std::vector<unsigned char>* out);

}  // namespace bl::io
