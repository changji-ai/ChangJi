// **The desktop shell's sources are not in this repository.**
//
// `desktop/qml` and `desktop/i18n` live in the private product repo. Seventeen
// test cases read them — the QML guards, and the half of the i18n checks that
// covers the interface — and every one of them used to open the directory with
// a bare `REQUIRE(fs::is_directory(...))`. In a standalone checkout that is
// seventeen hard failures on a clean clone, which says nothing about the code and
// makes the suite useless to anyone who does not have the other repository.
//
// So: skip when the sources are absent, and say so.
//
// ⚠️ **And guard the guard.** A test that can skip itself will eventually skip
// itself on the machine where it mattered — CI loses the product checkout for
// some unrelated reason, seventeen cases quietly stand down, and the run is
// green. So when the caller *knows* the sources should be there, it sets
// `CHANGJI_REQUIRE_DESKTOP=1` and their absence becomes a failure instead. The
// unit-test job sets it exactly when its (optional) product checkout succeeded.

#pragma once

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace changji_test {

/// `<product repo>/desktop`, derived from the QML directory CMake passes in.
inline std::filesystem::path desktop_dir() {
    return std::filesystem::path{CHANGJI_DESKTOP_QML_DIR}.parent_path();
}

/// Are the desktop sources beside this repository?
///
/// Returns false when they are not, after reporting the skip — the caller
/// returns from the test case. With `CHANGJI_REQUIRE_DESKTOP` set to anything
/// other than empty or `0`, their absence fails the test instead.
inline bool desktop_sources() {
    namespace fs = std::filesystem;
    const fs::path dir = desktop_dir();
    std::error_code ec;
    if (fs::is_directory(dir / "qml", ec)) return true;

    const char* must = std::getenv("CHANGJI_REQUIRE_DESKTOP");
    const std::string want = must ? must : "";
    if (!want.empty() && want != "0") {
        REQUIRE_MESSAGE(false,
                        "CHANGJI_REQUIRE_DESKTOP is set, so the desktop sources "
                        "were supposed to be at "
                            << dir.string()
                            << " — and they are not. Something dropped the "
                               "product-repo checkout, and seventeen test cases "
                               "were about to stand down quietly.");
        return false;
    }
    MESSAGE("skipped: the desktop sources are not beside this repository ("
            << dir.string()
            << "). They are in the private product repo; check it out next to "
               "this one to run these.");
    return false;
}

}  // namespace changji_test
