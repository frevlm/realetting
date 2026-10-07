# realetting

写好字段就能用的持久化配置。单头文件，C++23，基于 [nlohmann/json](https://github.com/nlohmann/json)。

```cpp
#include <realetting/realetting.hpp>

struct Settings {
    std::string model;
    std::string permission = "ask"; // 成员初始化器就是默认值
    int retries = 3;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Settings, model, permission, retries)

auto s = realetting::Store<Settings>::open(home / ".app" / "settings.json");
if (!s) return fail(s.error());

s->get().permission;                                  // 读：一份快照
s->update([](Settings &x) { x.model = "m-large"; });  // 写：改完即落盘
```

不用写文件读写、默认值合并、原子写、加锁。

## 行为

- **内存 = 默认值 ⊕ 文件。** 文件不存在不是错。文件不是 JSON 对象、或某个键类型不对，`open` 返回错误。
- **只写改动过的键。** 默认值不会渗进文件；用户写的、`Settings` 不认识的键原样保留。
- **不盖别人的改动。** `update` 每次都重读文件再改，手改过的键留着，内存也跟着文件走。
- **读不懂就不写。** 文件坏了，`update` 返回错误，文件和内存都不动。
- **不留半截文件。** 临时文件 + fsync（macOS 用 `F_FULLFSYNC`）+ rename + fsync 目录，进程被杀、断电都安全。
- **权限。** 新文件 `0600`（配置里常有密钥），已有文件保持原权限。
- **线程安全。** 同一个 `Store` 的 `get` / `update` 可以并发；`update` 的读—改—写整体持锁。

不做的事：跨进程加锁、热重载、Windows。

## 接入

```cmake
include(FetchContent)
FetchContent_Declare(realetting GIT_REPOSITORY https://github.com/frevlm/realetting.git GIT_TAG master)
FetchContent_MakeAvailable(realetting)
target_link_libraries(your_target PRIVATE realetting::realetting)
```

系统里找得到 nlohmann_json ≥ 3.11 就用系统的，找不到自动拉 3.12.0。

## 测试

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build
```
