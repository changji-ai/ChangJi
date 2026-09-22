// 对话 id 会变成文件名，所以它得先过一道。判据写在 `util/chat_id.hpp` 头上。

#include <doctest/doctest.h>

#include <string>

#include "util/chat_id.hpp"

using changji::util::chat_id_ok;

TEST_CASE("对话 id：正常那几个收下") {
    CHECK(chat_id_ok("c1789952769434"));
    CHECK(chat_id_ok("a"));
    CHECK(chat_id_ok("A-b_C-9"));
    CHECK(chat_id_ok(std::string(64, 'x')));
}

TEST_CASE("对话 id：能把文件写到别处去的一律拒") {
    // 这一条和片名那条是同一族：带 `..` 或者斜杠就能爬出项目目录。
    CHECK_FALSE(chat_id_ok(".."));
    CHECK_FALSE(chat_id_ok("../../etc/passwd"));
    CHECK_FALSE(chat_id_ok("a/b"));
    CHECK_FALSE(chat_id_ok("a\\b"));
    CHECK_FALSE(chat_id_ok("a.jsonl"));      // 点也不收：省得拼出别的后缀
    CHECK_FALSE(chat_id_ok("~"));
    CHECK_FALSE(chat_id_ok("a b"));
    CHECK_FALSE(chat_id_ok("对话一"));        // 非 ASCII 一律不收
}

TEST_CASE("对话 id：空的和太长的都不收") {
    // **空串不是"默认那一条"**：默认那一条在接口层面是"没带这个参数"，
    // 不是"带了一个空的"。两件事分开，省得"我明明传了"和"我没传"混成一件。
    CHECK_FALSE(chat_id_ok(""));
    CHECK_FALSE(chat_id_ok(std::string(65, 'x')));
}
