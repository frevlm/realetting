/*
 * realetting.hpp — 指定一个配置目录，拿到的 json 改了就自动写回文件
 *
 *   realetting::Dir cfg(home / ".app");
 *   auto s = cfg["settings.json"];               // 不存在就建，内容 {}
 *
 *   s["model"] = "m-large";                      // 即刻落盘，只动 /model 这一处
 *   s["mcp"]["fs"]["cmd"] = "node";              // 中间的对象自动建
 *   s["tags"]->push_back("x");                   // -> 后面是 nlohmann::json 的全部方法
 *   s->erase("model");                           // 从文件里删掉，回落到默认值
 *   std::string m = s["model"].get<std::string>();
 *   s["model"].path();                           // ".../settings.json#/model"
 *
 * 约定：
 * - 内存 = 默认值 ⊕ 文件（merge_patch：对象逐层合并，其余整个覆盖）。默认值只在内存里，
 *   文件里只出现写过的位置。默认值用 cfg.open(name, defaults) 给。
 * - 每次修改：锁住 → 重读文件 → 在副本上改 → 比较前后 → 只把差别写进文件。
 *   对象逐键比较，数组和标量整个写；没差别就不碰文件。别人手改过的地方不会被盖掉。
 * - 一句就是一次原子修改：-> 造出的临时对象持锁到这一句结束。这一句里抛了异常，修改作废。
 * - 文件读不懂、位置走不通（比如往字符串里放键）时抛 realetting::error，文件和内存都不变。
 * - 写入是 临时文件 + fsync + rename + fsync 目录：进程被杀、断电都不会留下半截文件。
 *   新建的文件权限 0600（配置里常有密钥），已有文件保持原权限。
 * - 同一个 Dir 打开同一个文件拿到的是同一份状态；读写都可以并发。跨进程不加锁。
 * - 别把 -> 的结果存起来（auto g = s.operator->()）：它会一直拿着锁。
 * - 只支持 POSIX。
 */
#pragma once

#include <cerrno>
#include <exception>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json.hpp"

namespace realetting {

using json = nlohmann::json;
namespace fs = std::filesystem;

struct error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

namespace detail {

[[noreturn]] inline void fail(const fs::path &p, std::string_view what)
{
    throw error(p.string() + ": " + std::string(what));
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

inline void write_atomic(const fs::path &target, std::string_view text)
{
    const fs::path dir = target.parent_path();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) fail(dir, ec.message());

    // 临时文件放同一目录：rename 只在同一文件系统内是原子的
    std::string tmp = (dir / (target.filename().string() + ".XXXXXX")).string();
    const int fd = ::mkstemp(tmp.data());
    if (fd < 0) fail(tmp, std::strerror(errno));

    if (struct stat st; ::stat(target.c_str(), &st) == 0) ::fchmod(fd, st.st_mode & 07777);
    bool ok = write_all(fd, text) && sync_fd(fd);
    ok = ::close(fd) == 0 && ok;
    ok = ok && ::rename(tmp.c_str(), target.c_str()) == 0;
    if (!ok)
    {
        const std::string why = std::strerror(errno);
        ::unlink(tmp.c_str());
        fail(target, why);
    }

    // rename 本身记在目录里，目录也要落盘
    if (const int d = ::open(dir.c_str(), O_RDONLY); d >= 0)
    {
        sync_fd(d);
        ::close(d);
    }
}

// 文件不存在就建一个 {}；打不开、不是 JSON 对象就报错（读不懂就别猜）
inline json read_tree(const fs::path &p)
{
    std::error_code ec;
    if (!fs::exists(p, ec))
    {
        write_atomic(p, "{}\n");
        return json::object();
    }
    std::ifstream f(p);
    if (!f) fail(p, "打不开");
    json j = json::parse(f, nullptr, false);
    if (!j.is_object()) fail(p, "不是 JSON 对象");
    return j;
}

inline void erase_at(json &tree, const json::json_pointer &ptr)
{
    if (ptr.empty()) tree = json::object();
    if (ptr.empty() || !tree.contains(ptr)) return;
    json &parent = tree[ptr.parent_pointer()];
    if (parent.is_object())
        parent.erase(ptr.back());
    else
        parent.erase(std::stoul(ptr.back()));
}

// 把 before → after 的差别写进 tree：对象逐键往下走，别的整个写
inline void apply_diff(json &tree, const json::json_pointer &ptr, const json &before, const json &after)
{
    if (!before.is_object() || !after.is_object())
    {
        if (before != after) tree[ptr] = after;
        return;
    }
    for (const auto &[k, v] : before.items())
        if (!after.contains(k)) erase_at(tree, ptr / k);
    for (const auto &[k, v] : after.items())
        apply_diff(tree, ptr / k, before.contains(k) ? before[k] : json(), v);
}

inline json at(const json &j, const json::json_pointer &ptr)
{
    return j.contains(ptr) ? j[ptr] : json();
}

class File {
  public:
    File(fs::path path, json defaults) : path_(std::move(path)), defaults_(std::move(defaults))
    {
        value_ = merged(read_tree(path_));
    }

    json get(const json::json_pointer &ptr) const
    {
        std::lock_guard lk(mutex_);
        return at(value_, ptr);
    }

    /* 重读文件，返回这个位置的最新值 */
    json fresh(const json::json_pointer &ptr)
    {
        std::lock_guard lk(mutex_);
        value_ = merged(read_tree(path_));
        return at(value_, ptr);
    }

    /* 重读文件 → change(文件树) → 落盘 → 内存换成 默认值 ⊕ 新文件。
     * 任何一步失败都抛 error，文件和内存不变。 */
    template <class F>
    void commit(F &&change)
    {
        std::lock_guard lk(mutex_);
        json tree = read_tree(path_);
        const json orig = tree;
        try
        {
            change(tree);
        }
        catch (const json::exception &e)
        {
            fail(path_, e.what());
        }
        json fresh = merged(tree);
        if (tree != orig) write_atomic(path_, tree.dump(2) + "\n");
        value_ = std::move(fresh);
    }

    std::recursive_mutex &mutex() { return mutex_; }
    const fs::path &path() const { return path_; }

  private:
    json merged(const json &tree) const
    {
        json v = defaults_.is_object() ? defaults_ : json::object();
        v.merge_patch(tree);
        return v;
    }

    const fs::path path_;
    const json defaults_;
    json value_;
    // 可重入：一句话里可以读写同一个文件好几处
    mutable std::recursive_mutex mutex_;
};

/* 一次修改：锁住文件，拿这个位置的最新副本给人改，析构时把差别写回。
 * 由 Ref::operator-> 造出来的临时对象，活到这一整句结束，所以一句就是一次原子修改。 */
class Edit {
  public:
    Edit(std::shared_ptr<File> file, json::json_pointer ptr)
        : file_(std::move(file)), ptr_(std::move(ptr)), lock_(file_->mutex()),
          before_(file_->fresh(ptr_)), after_(before_) {}
    Edit(const Edit &) = delete;
    Edit &operator=(const Edit &) = delete;

    ~Edit() noexcept(false)
    {
        // 这一句里别处抛了异常：放弃这次修改，也不能再抛
        if (std::uncaught_exceptions() > uncaught_ || after_ == before_) return;
        file_->commit([&](json &tree) { apply_diff(tree, ptr_, before_, after_); });
    }

    json *operator->() { return &after_; }
    json &operator*() { return after_; }

  private:
    std::shared_ptr<File> file_;
    json::json_pointer ptr_;
    std::unique_lock<std::recursive_mutex> lock_;
    int uncaught_ = std::uncaught_exceptions();
    json before_, after_;
};

} // namespace detail

/* 文件里的一个位置。读它读的是内存里那份 默认值 ⊕ 文件；
 * 改它（赋值，或 -> 调 nlohmann::json 的任何方法）就是改文件，只写改出来的差别。 */
class Ref {
  public:
    Ref operator[](std::string_view key) const { return Ref(file_, ptr_ / std::string(key)); }
    Ref operator[](size_t index) const { return Ref(file_, ptr_ / index); }

    Ref &operator=(json v)
    {
        *detail::Edit(file_, ptr_) = std::move(v);
        return *this;
    }
    // 和 vector<bool>::reference 一样：Ref 之间赋值是赋值，不是换绑
    Ref &operator=(const Ref &other) { return *this = other.get(); }
    Ref(const Ref &) = default;

    /* s["tags"]->push_back("x")、s->erase("model")、s->update({...}) */
    detail::Edit operator->() const { return detail::Edit(file_, ptr_); }

    /* 位置不存在时是 null */
    json get() const { return file_->get(ptr_); }
    template <class T>
    T get() const { return get().get<T>(); }

    std::string path() const { return file_->path().string() + "#" + ptr_.to_string(); }

  private:
    friend class Dir;
    Ref(std::shared_ptr<detail::File> file, json::json_pointer ptr)
        : file_(std::move(file)), ptr_(std::move(ptr)) {}

    std::shared_ptr<detail::File> file_;
    json::json_pointer ptr_;
};

/* 一个配置目录。不存在就建。 */
class Dir {
  public:
    explicit Dir(fs::path dir) : dir_(fs::absolute(std::move(dir)).lexically_normal())
    {
        std::error_code ec;
        fs::create_directories(dir_, ec);
        if (ec) detail::fail(dir_, ec.message());
    }

    Ref operator[](std::string_view name) { return open(name, json::object()); }

    /* defaults 只在第一次打开这个文件时生效：之后拿到的都是同一份状态 */
    Ref open(std::string_view name, json defaults)
    {
        const fs::path p = (dir_ / name).lexically_normal();
        std::lock_guard lk(mutex_);
        auto &f = files_[p.string()];
        if (!f) f = std::make_shared<detail::File>(p, std::move(defaults));
        return Ref(f, json::json_pointer());
    }

    const fs::path &path() const { return dir_; }

  private:
    const fs::path dir_;
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<detail::File>> files_;
};

} // namespace realetting
