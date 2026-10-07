# realetting

指定一个配置目录，拿到的 json 改了就自动写回文件。单头文件，C++17，nlohmann/json 3.12.0 已内置。

```cpp
#include <realetting/realetting.hpp>
using realetting::json;

realetting::Dir cfg(home / ".app");                 // 目录不存在就建
auto s = cfg["settings.json"];                      // 文件不存在就建，内容 {}

s["model"] = "m-large";                             // 即刻落盘，只动 /model 这一处
s["mcp"]["fs"]["cmd"] = "node";                     // 中间的对象自动建
std::string m = s["model"].get<std::string>();
s["model"].path();                                  // ".../settings.json#/model"
s["model"].erase();                                 // 从文件里删掉
s["tags"].edit([](json &j) { j.push_back("x"); });  // 复杂改动：改完写回差别
```

带默认值：

```cpp
auto s = cfg.open("settings.json", {{"permission", "ask"}, {"retries", 3}});
s["permission"].get<std::string>();  // "ask"，但文件里仍是 {}
```

## 行为

- **内存 = 默认值 ⊕ 文件。** 用 `merge_patch` 合并：对象逐层合并，其余整个覆盖。默认值只在内存里，文件里只出现写过的位置；`erase` 一个位置就回落到默认值。默认值只在第一次打开这个文件时生效。
- **不盖别人的改动。** 每次写都先重读文件，只改这一处。手改过的地方留着，内存也跟着文件走。
- **赋值写整个位置；`edit` 只写差别。** `edit` 比较前后，对象逐键往下走，数组和标量整个写；什么都没改就不碰文件。
- **出错抛 `realetting::error`，什么都不变。** 文件不是 JSON 对象、位置走不通（比如往字符串里放键），文件和内存都保持原样。
- **不留半截文件。** 临时文件 + fsync（macOS 用 `F_FULLFSYNC`）+ rename + fsync 目录，进程被杀、断电都安全。
- **权限。** 新文件 `0600`（配置里常有密钥），已有文件保持原权限。
- **线程安全。** 同一个 `Dir` 打开同一个文件（`a.json` 和 `./a.json` 算同一个）拿到同一份状态，读写可以并发；`Ref` 可以活得比 `Dir` 久。

不做的事：跨进程加锁、监听文件变化、Windows。

## 接入

```cmake
include(FetchContent)
FetchContent_Declare(realetting GIT_REPOSITORY https://github.com/frevlm/realetting.git GIT_TAG master)
FetchContent_MakeAvailable(realetting)
target_link_libraries(your_target PRIVATE realetting::realetting)
```

或者直接把 `include/realetting/` 拷进项目。

## 测试

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build
```
