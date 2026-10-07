#include <realetting/realetting.hpp>

#include <cstdio>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using realetting::json;

static int failures = 0;
#define CHECK(cond, msg)                                            \
    do                                                              \
    {                                                               \
        if (cond)                                                   \
            printf("  ok   %s\n", msg);                             \
        else                                                        \
        {                                                           \
            printf("  FAIL %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++failures;                                             \
        }                                                           \
    } while (0)

template <class F>
static bool throws(F &&f)
{
    try
    {
        f();
    } catch (const realetting::error &)
    {
        return true;
    }
    return false;
}

static const fs::path dir = fs::temp_directory_path() / "realetting_test";
static const fs::path file = dir / "settings.json";

static void put(const std::string &text)
{
    fs::create_directories(dir);
    std::ofstream(file) << text;
}

static std::string raw()
{
    std::ifstream f(file);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static json on_disk() { return json::parse(raw(), nullptr, false); }

static void reset() { fs::remove_all(dir); }

static int files_in_dir()
{
    int n = 0;
    for ([[maybe_unused]] const auto &e : fs::directory_iterator(dir)) ++n;
    return n;
}

static const json defaults = {{"permission", "ask"}, {"retries", 3}, {"tags", json::array()}, {"mcp", {{"fs", {{"cmd", "cat"}}}}}};

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("== 打开 ==\n");
    {
        reset();
        realetting::Dir cfg(dir / "a" / "b");
        CHECK(fs::is_directory(dir / "a" / "b"), "配置目录不存在就建");
    }
    {
        reset();
        realetting::Dir cfg(dir);
        auto s = cfg["settings.json"];
        CHECK(on_disk() == json::object(), "文件不存在就建，内容 {}");
        CHECK(s.get() == json::object(), "内存里也是 {}");
    }
    {
        reset();
        put(R"({"model":"m","extra":1})");
        realetting::Dir cfg(dir);
        auto s = cfg.open("settings.json", defaults);
        CHECK(s["model"].get<std::string>() == "m", "读到文件里的值");
        CHECK(s["permission"].get<std::string>() == "ask", "缺的取默认值");
        CHECK(s["mcp"]["fs"]["cmd"].get<std::string>() == "cat", "嵌套的默认值也在");
        CHECK(s["nope"].get().is_null(), "不存在的位置是 null");
        CHECK(raw() == R"({"model":"m","extra":1})", "只读不改文件");
    }
    {
        reset();
        realetting::Dir cfg(dir);
        put("{ not json");
        CHECK(throws([&] { cfg["settings.json"]; }), "坏 JSON 抛错");
        CHECK(raw() == "{ not json", "坏文件原样保留");
        put("[1,2]");
        CHECK(throws([&] { cfg["settings.json"]; }), "不是对象抛错");
        put("{}");
        CHECK(!throws([&] { cfg["settings.json"]; }), "修好了再开就行");
    }

    printf("== 赋值 ==\n");
    {
        reset();
        realetting::Dir cfg(dir);
        auto s = cfg.open("settings.json", defaults);
        s["model"] = "m";
        CHECK(on_disk() == json({{"model", "m"}}), "文件里只有写过的位置，默认值没渗进去");
        CHECK(s["model"].get<std::string>() == "m", "内存跟着改了");
        s["mcp"]["web"]["cmd"] = "node";
        CHECK(on_disk()["mcp"] == json({{"web", {{"cmd", "node"}}}}), "嵌套位置，中间对象自动建");
        CHECK(s["mcp"]["fs"]["cmd"].get<std::string>() == "cat", "默认值与文件逐层合并");
        CHECK(s["mcp"]["web"]["cmd"].path() == (dir / "settings.json").string() + "#/mcp/web/cmd",
              "位置是 文件#JSON pointer");
        s["retries"] = s["model"];
        CHECK(on_disk()["retries"] == "m", "Ref 之间赋值是拷值");
    }
    {
        reset();
        realetting::Dir cfg(dir);
        auto s = cfg["settings.json"];
        put(R"({"external":1})");
        s["model"] = "m";
        CHECK(on_disk() == json({{"external", 1}, {"model", "m"}}), "别人手改的地方没被盖掉");
        CHECK(s["external"].get<int>() == 1, "内存也拿到了别人的改动");
    }
    {
        reset();
        realetting::Dir cfg(dir);
        auto s = cfg["settings.json"];
        s["model"] = "m";
        put("{ broken");
        CHECK(throws([&] { s["model"] = "n"; }), "文件读不懂就抛错");
        CHECK(raw() == "{ broken", "坏文件原样保留");
        CHECK(s["model"].get<std::string>() == "m", "内存没变");
        put(R"({"model":"m"})");
        CHECK(throws([&] { s["model"]["x"] = 1; }), "位置走不通也抛错");
        CHECK(on_disk() == json({{"model", "m"}}), "文件没动");
    }

    printf("== -> 改动走 diff ==\n");
    {
        reset();
        realetting::Dir cfg(dir);
        auto s = cfg.open("settings.json", defaults);
        s["tags"]->push_back("x");
        CHECK(on_disk() == json({{"tags", {"x"}}}), "数组上 push_back，整个数组写进去");
        s->update({{"retries", 5}, {"permission", "ask"}});
        CHECK(on_disk() == json({{"tags", {"x"}}, {"retries", 5}}), "根上 update：和默认值一样的那个键没写");
        s["mcp"] = {{"fs", {{"cmd", "cat"}}}, {"web", {{"cmd", "node"}}}};
        CHECK(on_disk()["mcp"] == json({{"web", {{"cmd", "node"}}}}), "赋值也走 diff：只写和默认值不同的");
        s->erase("retries");
        CHECK(!on_disk().contains("retries") && s["retries"].get<int>() == 3, "erase 从文件删掉、回落到默认值");
        s->erase("nope");
        s["mcp"]["web"]->erase("cmd");
        CHECK(on_disk()["mcp"]["web"] == json::object(), "嵌套位置上也能 erase");
        CHECK(s["tags"]->size() == 1, "-> 也能读");
        put(on_disk().dump()); // 换成紧凑格式：要是又写了一遍，会变回缩进的
        const std::string compact = raw();
        s["tags"]->size();
        s["model"] = s["model"];
        CHECK(raw() == compact, "什么都没改就不碰文件");
    }
    {
        reset();
        realetting::Dir cfg(dir);
        auto s = cfg["settings.json"];
        const auto boom = []() -> std::string { throw std::runtime_error("boom"); };
        try
        {
            s["tags"]->push_back(boom());
        }
        catch (const std::runtime_error &)
        {
        }
        CHECK(on_disk() == json::object(), "这一句里抛了异常，修改作废");
        s["a"]->push_back(s["b"].get());
        CHECK(on_disk() == json({{"a", {nullptr}}}), "一句里读写同一个文件好几处不死锁");
    }

    printf("== 文件 ==\n");
    {
        reset();
        realetting::Dir cfg(dir);
        auto s = cfg["settings.json"];
        CHECK((fs::status(file).permissions() & fs::perms::all) == (fs::perms::owner_read | fs::perms::owner_write),
              "新文件 0600");
        fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write | fs::perms::others_read);
        s["model"] = "m";
        CHECK((fs::status(file).permissions() & fs::perms::others_read) != fs::perms::none, "已有文件保持原权限");
        CHECK(files_in_dir() == 1, "没留下临时文件");
        cfg["sub/x.json"]["k"] = 1;
        CHECK(fs::exists(dir / "sub" / "x.json"), "名字里带子目录也行");
    }
    {
        reset();
        std::optional<realetting::Ref> s;
        {
            realetting::Dir cfg(dir);
            s = cfg["settings.json"];
        }
        (*s)["model"] = "m";
        CHECK(on_disk()["model"] == "m", "Dir 没了 Ref 照样能用");
    }

    printf("== 并发 ==\n");
    {
        reset();
        realetting::Dir cfg(dir);
        constexpr int kThreads = 8, kEach = 25;
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; ++t)
            ts.emplace_back([&cfg, t] {
                // 两种写法指向同一个文件，拿到的是同一份状态
                auto s = cfg[t % 2 ? "settings.json" : "./settings.json"];
                for (int i = 0; i < kEach; ++i) s["tags"]->push_back(t);
            });
        for (auto &t : ts) t.join();
        CHECK(on_disk()["tags"].size() == kThreads * kEach, "一次都没丢");
    }

    reset();
    printf(failures == 0 ? "\nPASS\n" : "\nFAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
