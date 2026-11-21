// 进程与文件的落点：exe 所在目录就是设置文件、导出文件的默认根。
#pragma once

#include <filesystem>
#include <string>

namespace paths {

// exe 所在目录，带结尾反斜杠之外的分隔符规范化（不含末尾分隔符）。
const std::string& ExeDir();
// exe 完整路径。
const std::string& ExePath();
// 拼到 exe 目录下的一个文件名。
std::string BesideExe(const std::string& filename);

// UTF-8 字符串 ⇄ std::filesystem::path。
// MSVC 的 path(char*) 按 ANSI 解，中文路径会变乱码，所以本工程只用这两个入口。
std::filesystem::path Utf8Path(const std::string& utf8);
std::string Utf8String(const std::filesystem::path& path);

}  // namespace paths
