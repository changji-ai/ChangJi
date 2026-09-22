// default_doctor 单独一个文件。
//
// 它是这一组接口里唯一要链 doctor.cpp（进而链 httplib）的东西，
// 而 config_api.cpp 的其余部分全是纯逻辑，要进单元测试目标。
// 和 llm/client_http.cpp 分出来是同一个道理。

#include "doctor/doctor.hpp"
#include "http/config_api.hpp"

namespace changji::http {

DoctorFn default_doctor() {
    return [](const config::Settings& s) { return doctor::run_checks(s); };
}

}  // namespace changji::http
