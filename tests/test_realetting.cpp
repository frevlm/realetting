#include <realetting/realetting.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

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

struct Settings {
    std::string model;
    std::string permission = "ask";
    int retries = 3;
    std::vector<std::string> tags;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Settings, model, permission, retries, tags)

using Store = realetting::Store<Settings>;

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

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("== open ==\n");
    {
        reset();
        auto s = Store::open(file);
        CHECK(s.has_value(), "文件不存在不是错");
        CHECK(s->get().permission == "ask" && s->get().retries == 3, "全是默认值");
        CHECK(!fs::exists(file), "只读不建文件");
    }
    {
        reset();
        put(R"({"model":"m"})");
        auto s = Store::open(file);
        CHECK(s && s->get().model == "m" && s->get().permission == "ask", "文件覆盖，缺的取默认值");
    }
    {
        reset();
        put("{ not json");
        CHECK(!Store::open(file).has_value(), "坏 JSON 报错");
        put("[1,2]");
        CHECK(!Store::open(file).has_value(), "不是对象报错");
        put(R"({"retries":"x"})");
        CHECK(!Store::open(file).has_value(), "类型不对报错");
    }

    printf("== update ==\n");
    {
        reset();
        auto s = Store::open(file);
        CHECK(s->update([](Settings &x) { x.model = "m"; }).has_value(), "写成功");
        CHECK(on_disk() == json({{"model", "m"}}), "文件里只有改过的键，默认值没渗进去");
        CHECK(s->get().model == "m", "内存跟着改了");
    }
    {
        reset();
        put(R"({"model":"a","extra":1})");
        auto s = Store::open(file);
        s->update([](Settings &x) { x.retries = 5; });
        CHECK(on_disk() == json({{"model", "a"}, {"extra", 1}, {"retries", 5}}),
              "不认识的键和用户原有的键都还在");
    }
    {
        reset();
        auto s = Store::open(file);
        put(R"({"model":"external"})");
        s->update([](Settings &x) { x.permission = "deny"; });
        CHECK(on_disk() == json({{"model", "external"}, {"permission", "deny"}}),
              "别人手改的键没被盖掉");
        CHECK(s->get().model == "external", "内存也拿到了别人的改动");
    }
    {
        reset();
        auto s = Store::open(file);
        put("{ broken");
        CHECK(!s->update([](Settings &x) { x.model = "m"; }).has_value(), "文件读不懂就拒写");
        CHECK(raw() == "{ broken", "用户的坏文件原样保留");
        CHECK(s->get().model.empty(), "内存也没变");
    }
    {
        reset();
        auto s = Store::open(file);
        put(R"({"retries":"x"})");
        CHECK(!s->update([](Settings &x) { x.model = "m"; }).has_value(), "改完的文件类型不对也拒写");
        CHECK(raw() == R"({"retries":"x"})", "文件没动");
    }
    {
        reset();
        auto s = Store::open(file);
        s->update([](Settings &x) { x.permission = "ask"; });
        CHECK(!fs::exists(file), "没改动就不写");
    }

    printf("== 文件 ==\n");
    {
        reset();
        auto s = Store::open(file);
        s->update([](Settings &x) { x.model = "m"; });
        CHECK((fs::status(file).permissions() & fs::perms::all) == (fs::perms::owner_read | fs::perms::owner_write),
              "新文件 0600");
        fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read);
        s->update([](Settings &x) { x.model = "n"; });
        CHECK((fs::status(file).permissions() & fs::perms::others_read) != fs::perms::none, "已有文件保持原权限");
        CHECK(files_in_dir() == 1, "没留下临时文件");
    }
    {
        reset();
        auto s = Store::open(dir / "a" / "b" / "settings.json");
        CHECK(s->update([](Settings &x) { x.model = "m"; }).has_value() &&
                  fs::exists(dir / "a" / "b" / "settings.json"),
              "目录不存在就建");
    }

    printf("== 并发 ==\n");
    {
        reset();
        auto s = Store::open(file);
        constexpr int kThreads = 8, kEach = 25;
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; ++t)
            ts.emplace_back([&s, t] {
                for (int i = 0; i < kEach; ++i)
                    s->update([&](Settings &x) {
                        x.retries += 1;
                        x.tags.push_back(std::to_string(t));
                    });
            });
        for (auto &t : ts) t.join();
        CHECK(s->get().retries == 3 + kThreads * kEach, "内存一次都没丢");
        CHECK(on_disk()["tags"].size() == kThreads * kEach, "文件一次都没丢");
    }

    reset();
    printf(failures == 0 ? "\nPASS\n" : "\nFAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
