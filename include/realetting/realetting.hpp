/*
 * realetting.hpp — 写好字段就能用的持久化配置
 *
 *   struct Settings {
 *       std::string model;
 *       std::string permission = "ask";   // 成员初始化器就是默认值
 *   };
 *   NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Settings, model, permission)
 *
 *   auto s = realetting::Store<Settings>::open(home / ".app" / "settings.json");
 *   s->get().permission;                                  // 读：一份快照
 *   s->update([](Settings &x) { x.model = "m-large"; });  // 写：改完即落盘
 *
 * 约定：
 * - 内存 = 默认值 ⊕ 文件。文件不存在不是错；文件不是 JSON 对象、或某个键类型不对，open 报错。
 * - update 只把改动过的键写进文件：默认值不渗进去，用户写的、本结构不认识的键原样保留。
 * - update 每次重读文件再改，所以别人手改过的键不会被盖掉，改完内存也跟着文件走。
 * - 文件读不懂时 update 拒绝写入，不覆盖用户数据。
 * - 写入是 临时文件 + fsync + rename + fsync 目录：进程被杀、断电都不会留下半截文件。
 *   新建的文件权限 0600（配置里常有密钥），已有文件保持原权限。
 * - 线程安全：同一个 Store 的 get / update 可以并发。跨进程不加锁。
 * - 只支持 POSIX。
 */
#pragma once

#include <cerrno>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace realetting {

namespace detail {

namespace fs = std::filesystem;

inline std::unexpected<std::string> error(const fs::path &p, std::string_view what)
{
    return std::unexpected(p.string() + ": " + std::string(what));
}

// 文件不存在 → 空对象；打不开、不是 JSON 对象 → 错误（读不懂就别猜）
inline std::expected<nlohmann::json, std::string> read_tree(const fs::path &p)
{
    std::error_code ec;
    if (!fs::exists(p, ec)) return nlohmann::json::object();
    std::ifstream f(p);
    if (!f) return error(p, "打不开");
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object()) return error(p, "不是 JSON 对象");
    return j;
}

// 缺的键取默认值（WITH_DEFAULT 宏），类型不对的键报错
template <class T>
std::expected<T, std::string> decode(const nlohmann::json &tree, const fs::path &p)
{
    try
    {
        return tree.get<T>();
    } catch (const nlohmann::json::exception &e)
    {
        return error(p, e.what());
    }
}

// macOS 上 fsync 只交给磁盘缓存，F_FULLFSYNC 才落到盘上
inline bool sync_fd(int fd)
{
#ifdef F_FULLFSYNC
    if (::fcntl(fd, F_FULLFSYNC) == 0) return true;
#endif
    return ::fsync(fd) == 0;
}

inline bool write_all(int fd, std::string_view s)
{
    while (!s.empty())
    {
        const ssize_t n = ::write(fd, s.data(), s.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        s.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

inline std::expected<void, std::string> write_atomic(const fs::path &target, std::string_view text)
{
    const fs::path dir = target.has_parent_path() ? target.parent_path() : fs::path(".");
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return error(dir, ec.message());

    // 临时文件放同一目录：rename 只在同一文件系统内是原子的
    std::string tmp = (dir / (target.filename().string() + ".XXXXXX")).string();
    const int fd = ::mkstemp(tmp.data());
    if (fd < 0) return error(tmp, std::strerror(errno));

    if (struct stat st; ::stat(target.c_str(), &st) == 0) ::fchmod(fd, st.st_mode & 07777);
    bool ok = write_all(fd, text) && sync_fd(fd);
    ok = ::close(fd) == 0 && ok;
    ok = ok && ::rename(tmp.c_str(), target.c_str()) == 0;
    if (!ok)
    {
        const std::string why = std::strerror(errno);
        ::unlink(tmp.c_str());
        return error(target, why);
    }

    // rename 本身记在目录里，目录也要落盘
    if (const int d = ::open(dir.c_str(), O_RDONLY); d >= 0)
    {
        sync_fd(d);
        ::close(d);
    }
    return {};
}

} // namespace detail

template <class T>
class Store {
  public:
    static std::expected<Store, std::string> open(std::filesystem::path file)
    {
        auto tree = detail::read_tree(file);
        if (!tree) return std::unexpected(tree.error());
        auto value = detail::decode<T>(*tree, file);
        if (!value) return std::unexpected(value.error());
        return Store(std::move(file), std::move(*value));
    }

    T get() const
    {
        std::lock_guard lk(*mutex_);
        return value_;
    }

    /* change 改一份副本；改动过的键写进文件，成功后内存换成 默认值 ⊕ 新文件。
     * 失败时文件和内存都不变。 */
    template <class F>
    std::expected<void, std::string> update(F &&change)
    {
        std::lock_guard lk(*mutex_);
        T next = value_;
        std::invoke(std::forward<F>(change), next);

        const nlohmann::json before = value_, after = next;
        auto tree = detail::read_tree(path_);
        if (!tree) return std::unexpected(tree.error());
        bool dirty = false;
        for (const auto &[k, v] : after.items())
        {
            const auto it = before.find(k);
            if (it != before.end() && *it == v) continue;
            (*tree)[k] = v;
            dirty = true;
        }
        if (!dirty) return {};

        auto fresh = detail::decode<T>(*tree, path_);
        if (!fresh) return std::unexpected(fresh.error());
        if (auto w = detail::write_atomic(path_, tree->dump(2) + "\n"); !w) return w;
        value_ = std::move(*fresh);
        return {};
    }

    const std::filesystem::path &path() const { return path_; }

  private:
    Store(std::filesystem::path path, T value)
        : path_(std::move(path)), value_(std::move(value)) {}

    std::filesystem::path path_;
    T value_;
    // mutex 不可移动，包一层让 open 能按值返回
    std::unique_ptr<std::mutex> mutex_ = std::make_unique<std::mutex>();
};

} // namespace realetting
