// Crow 解 multipart，一段没有 name（cmake/patch_crow_multipart_name.cmake，2026-09-25）。
//
// Crow 1.2.0 拿 name 那一句是 `params.find("name")->second`，不查 end()。一段
// 没有 Content-Disposition 的上传就把整个引擎打崩（SEGV，guard() 接不住）。
// 三条上传路由（参考图、场景图、参考音色）都走这个构造函数。
//
// 没打补丁时这条用例不是红，是**整个测试程序崩掉**——那也算响。

#include <doctest/doctest.h>

#include <crow.h>

#include <string>

namespace {

crow::request multipart_request(const std::string& body) {
    crow::request req;
    req.method = crow::HTTPMethod::Post;
    req.add_header("Content-Type", "multipart/form-data; boundary=X");
    req.body = body;
    return req;
}

}  // namespace

TEST_CASE("multipart：一段没有 Content-Disposition，不崩，也取不到 file") {
    const auto req = multipart_request("--X\r\nFoo: bar\r\n\r\nabc\r\n--X--\r\n");
    crow::multipart::message msg(req);
    CHECK(msg.part_map.find("file") == msg.part_map.end());
}

TEST_CASE("multipart：有 Content-Disposition 但不带 name，不崩") {
    const auto req = multipart_request(
        "--X\r\nContent-Disposition: form-data; filename=\"a.png\"\r\n\r\nabc\r\n--X--\r\n");
    crow::multipart::message msg(req);
    CHECK(msg.part_map.find("file") == msg.part_map.end());
}

TEST_CASE("multipart：连头和正文之间那个空行都没有，不崩") {
    const auto req = multipart_request("--X\r\nabc\r\n--X--\r\n");
    crow::multipart::message msg(req);
    CHECK(msg.part_map.find("file") == msg.part_map.end());
}

TEST_CASE("multipart：正常的那种照旧按名字取得到") {
    const auto req = multipart_request(
        "--X\r\nContent-Disposition: form-data; name=\"project\"\r\n\r\n/p\r\n"
        "--X\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.png\"\r\n"
        "Content-Type: image/png\r\n\r\nPNGDATA\r\n--X--\r\n");
    crow::multipart::message msg(req);
    REQUIRE(msg.part_map.count("project") == 1);
    CHECK(msg.part_map.find("project")->second.body == "/p");
    REQUIRE(msg.part_map.count("file") == 1);
    CHECK(msg.part_map.find("file")->second.body == "PNGDATA");
    CHECK(msg.part_map.find("file")->second.get_header_object("Content-Type").value ==
          "image/png");
}
