// 按内容存的小仓库。跨机派活时输入文件走它。
//
// 两件事必须钉死，而且都是"错了也不报错"的那种：
//
//   一，**指纹串会被拼进文件路径**。不查格式的话，`../../etc/passwd`
//       就写到别处去了。这是安全边界，和 http/media.hpp 那边的越界检查
//       同一类。
//   二，**截断的文件要在落地之前被挡住**。png 少几个字节照样"存下来了"，
//       之后报的是「权重读不对」或者一张半截图，指向完全错误的方向。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "infer/blob.hpp"
#include "util/text.hpp"

using namespace changji::infer;
namespace fs = std::filesystem;

namespace {

fs::path fresh_root(const char* tag) {
    const auto root = fs::temp_directory_path() / "changji_blob_test" / tag;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

std::string write_file(const fs::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return changji::text::sha1_hex(bytes);
}

}  // namespace

TEST_CASE("指纹的格式是一道安全边界") {
    CHECK(blob_id_ok("da39a3ee5e6b4b0d3255bfef95601890afd80709"));

    CHECK_FALSE(blob_id_ok(""));
    CHECK_FALSE(blob_id_ok("da39a3ee"));                       // 短
    CHECK_FALSE(blob_id_ok(std::string(41, 'a')));             // 长
    CHECK_FALSE(blob_id_ok("DA39A3EE5E6B4B0D3255BFEF95601890AFD80709"));  // 大写
    CHECK_FALSE(blob_id_ok("../../../etc/passwd/aaaaaaaaaaaaaaaaaaaa"));
    CHECK_FALSE(blob_id_ok("da39a3ee5e6b4b0d3255bfef95601890afd8070g"));  // 非十六进制
}

TEST_CASE("非法指纹拼不出路径") {
    // 回空路径，调用方必须先判空——这是上一条那道边界的落地点。
    const auto root = fs::temp_directory_path() / "changji_blob_test";
    CHECK(blob_path(root, "../../etc/passwd").empty());
    CHECK(blob_path(root, "").empty());

    const std::string id = "da39a3ee5e6b4b0d3255bfef95601890afd80709";
    const auto p = blob_path(root, id);
    REQUIRE_FALSE(p.empty());
    // 前两位切一层子目录：几千个 blob 平铺在一个目录里，Windows 上
    // 光是列目录就要好几秒
    CHECK(p.parent_path().filename().string() == "da");
    CHECK(p.filename().string() == id.substr(2));
}

TEST_CASE("存进去、认得出、不重复存") {
    const auto root = fresh_root("store");
    const std::string bytes = "一段假装是参考图的内容";
    const std::string id = changji::text::sha1_hex(bytes);

    CHECK_FALSE(blob_present(root, id));
    CHECK(blob_store(root, id, bytes).empty());
    CHECK(blob_present(root, id));

    // 再存一次：本来就有，直接成功，不报错也不重写
    CHECK(blob_store(root, id, bytes).empty());
}

TEST_CASE("内容对不上指纹就不落地") {
    // 传了一半是这条路上最阴的故障：文件名对、内容半截，而之后每一次
    // blob_present 都会说它在，再也不会重传。
    const auto root = fresh_root("truncated");
    const std::string whole = "完整的那一份内容，比较长一点";
    const std::string id = changji::text::sha1_hex(whole);
    const std::string half = whole.substr(0, whole.size() / 2);

    const std::string why = blob_store(root, id, half);
    CHECK_FALSE(why.empty());
    CHECK(why.find("对不上") != std::string::npos);
    CHECK(why.find(std::to_string(half.size())) != std::string::npos);
    CHECK_FALSE(blob_present(root, id));   // 最要紧的一条：没落地
}

TEST_CASE("非法指纹存不进去") {
    const auto root = fresh_root("badid");
    CHECK_FALSE(blob_store(root, "../evil", "whatever").empty());
    CHECK_FALSE(blob_store(root, "", "whatever").empty());
}

TEST_CASE("认领一个已有的文件") {
    const auto root = fresh_root("adopt");
    const auto src = root / "ref_front.png";
    const std::string want = write_file(src, "假装这是一张定妆图");

    const std::string id = blob_adopt(root, src);
    CHECK(id == want);
    CHECK(blob_present(root, id));
    CHECK(blob_id_of(src) == want);
}

TEST_CASE("同样的内容只占一份") {
    // 一章 22 镜、每镜三五张参考图，而那几张是同一批文件。按路径传
    // 就是同一张脸传二十二遍，按内容寻址第二镜起对面回一句"我有了"。
    const auto root = fresh_root("dedup");
    const auto a = root / "a.png";
    const auto b = root / "b.png";
    write_file(a, "同一张脸");
    write_file(b, "同一张脸");

    CHECK(blob_adopt(root, a) == blob_adopt(root, b));
}
