// **Every path the CI yml files mention has to be a file that exists.**
//
// A rename, a move, a typo — that class of mistake **is only discovered by
// pushing**, and the way you discover it is a build stopping halfway through on
// "No such file or directory". The desktop pipeline is worse: it installs Qt
// and builds sd.cpp and llama.cpp, tens of minutes across three platforms,
// before it reaches that line.
//
// This test costs milliseconds.
//
// It accepts both spellings: `changji/cpp/...` (Unix) and `changji\cpp\...`
// (which is how the Windows steps in the yml write it). Anything containing
// `${{ }}` is skipped — that is a template, and what it expands to is not
// knowable here.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace fs = std::filesystem;

namespace {

/// This repository's root: `src/`, `tests/`, `tools/`, `.github/` are all
/// directly inside it. `CHANGJI_SRC_DIR` is `<root>/src`, so: **one level up.**
///
/// ⚠️ **It used to go up two levels**, back when this engine was a `cpp/`
/// subdirectory of the product repo and the workflows lived in that repo's
/// root. In the standalone repository that lands *outside the checkout
/// entirely*, and the first REQUIRE stopped the whole test — which is exactly
/// how it was failing on Linux before the workflows moved here. Red looked
/// like "a CI path is wrong"; in fact the test could not find itself, and
/// while it was stopped, every mistake it exists to catch went through.
fs::path repo_root() { return fs::path{CHANGJI_SRC_DIR}.parent_path(); }

/// The private product repo, which holds `webapp/`, `desktop/` and `brand/`.
///
/// It is **not** checked out next to this one in general, so the paths that
/// point into it can only be verified when it happens to be there — which is
/// the case in the nested working copy, where this repo sits inside it. When
/// it is absent those paths are counted and reported, not silently dropped:
/// "skipped 4" is information; a quietly shrinking count is not.
fs::path product_root() { return repo_root().parent_path(); }
bool product_present() {
    return fs::is_directory(product_root() / "webapp") &&
           fs::is_directory(product_root() / "desktop");
}

/// The top-level directories of this repository. Used to recognise the
/// repo-relative paths that cloud-tool.yml writes (`tools/autodl_gui.py`), and
/// derived from the tree rather than listed by hand so it cannot go stale.
const std::set<std::string>& top_level() {
    static const std::set<std::string> dirs = [] {
        std::set<std::string> d;
        for (const auto& e : fs::directory_iterator(repo_root())) {
            if (e.is_directory()) d.insert(e.path().filename().string());
        }
        return d;
    }();
    return dirs;
}

/// Does this token look like a path into one of the two repositories?
///
/// Three shapes are accepted, and nothing else — a relative `../desktop/...`
/// inside a comment, or a system path like `/usr/bin/...`, is not this test's
/// business and accepting it would only produce false alarms.
bool looks_like_path(const std::string& t) {
    if (t.find("${{") != std::string::npos) return false;   // a template
    if (t.find('*') != std::string::npos) return false;     // a glob
    if (t.find('{') != std::string::npos) return false;     // `package_{a,b}.sh`
    if (t.find('/') == std::string::npos) return false;     // not a path at all
    if (t.rfind("changji/", 0) == 0) return true;
    const std::string head = t.substr(0, t.find('/'));
    return top_level().count(head) > 0;
}

enum class Where { kHere, kProduct };

/// Turn a token from a yml file into a real path.
///
/// ⚠️ **The workspace layout the yml files are written against:**
///
///     changji/          the private product repo (webapp/, desktop/, brand/)
///     changji/cpp/      THIS repo, laid on top of the copy in there
///
/// So `changji/cpp/X` is this repo's `X`, while `changji/Y` is the product
/// repo's `Y`. Everything else is already relative to this repo's root.
std::pair<fs::path, Where> resolve_ci(const std::string& p) {
    static const std::string kProduct = "changji/";
    static const std::string kHere = "changji/cpp/";
    if (p.rfind(kHere, 0) == 0) {
        return {repo_root() / p.substr(kHere.size()), Where::kHere};
    }
    if (p.rfind(kProduct, 0) == 0) {
        return {product_root() / p.substr(kProduct.size()), Where::kProduct};
    }
    return {repo_root() / p, Where::kHere};
}

/// Cut a yml file into tokens that might be paths.
///
/// Backslashes become slashes (that is how the Windows steps spell them), and
/// surrounding quotes, commas and brackets are stripped.
std::set<std::string> paths_in(const fs::path& f) {
    std::ifstream in(f);
    REQUIRE_MESSAGE(in.good(), "cannot read " << f.string());
    std::set<std::string> out;
    for (std::string line; std::getline(in, line);) {
        std::string tok;
        for (std::size_t i = 0; i <= line.size(); ++i) {
            const char c = i < line.size() ? line[i] : ' ';
            const bool sep = c == ' ' || c == '\t' || c == '"' || c == '\'' ||
                             c == ',' || c == '(' || c == ')' || c == '`' ||
                             c == '\r';
            if (!sep) { tok += (c == '\\' ? '/' : c); continue; }
            if (!tok.empty()) {
                // Strip trailing punctuation: `path.`, `path:`, `path;`
                while (!tok.empty() &&
                       (tok.back() == ':' || tok.back() == ';' || tok.back() == '.')) {
                    tok.pop_back();
                }
                // `./.github/...` → `.github/...`
                if (tok.rfind("./", 0) == 0) tok = tok.substr(2);
                if (looks_like_path(tok)) out.insert(tok);
            }
            tok.clear();
        }
    }
    return out;
}

}  // namespace

TEST_CASE("every path the CI yml files mention really exists") {
    const fs::path dir = repo_root() / ".github" / "workflows";
    REQUIRE_MESSAGE(fs::is_directory(dir), "cannot read " << dir.string());

    const bool have_product = product_present();
    int checked = 0;
    int skipped = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".yml") continue;
        for (const auto& p : paths_in(entry.path())) {
            const auto [full, where] = resolve_ci(p);
            if (where == Where::kProduct && !have_product) { ++skipped; continue; }
            ++checked;
            CAPTURE(entry.path().filename().string());
            CAPTURE(p);
            CHECK_MESSAGE(fs::exists(full), "nothing at this path: " << full.string());
        }
    }

    // `std::string`, not a ternary of two `const char*` — doctest's
    // stringification takes the latter as a pointer and prints an address.
    MESSAGE("checked " << checked << " paths, skipped " << skipped
                       << " (product repo "
                       << std::string(have_product ? "present" : "absent") << ")");

    // Extracting nothing means the test stopped recognising paths, not that
    // everything is correct.
    REQUIRE_MESSAGE(checked >= 10,
                    "only extracted " << checked << " paths — this test has most "
                    "likely stopped recognising them");
}
