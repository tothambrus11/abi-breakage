#include "hdr/index.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <regex>
#include <set>
#include <string_view>

#include <clang-c/Index.h>

#include "core/contracts.hpp"
#include "core/fs.hpp"
#include "core/hash.hpp"

namespace abistudy::hdr {
namespace {

// ----------------------------------------------------------------------------
// RAII over libclang C objects
// ----------------------------------------------------------------------------

struct IndexDeleter {
  void operator()(CXIndex i) const noexcept {
    if (i)
      clang_disposeIndex(i);
  }
};
struct TuDeleter {
  void operator()(CXTranslationUnit t) const noexcept {
    if (t)
      clang_disposeTranslationUnit(t);
  }
};
using Index = std::unique_ptr<std::remove_pointer_t<CXIndex>, IndexDeleter>;
using Tu = std::unique_ptr<std::remove_pointer_t<CXTranslationUnit>, TuDeleter>;

/// @brief Takes ownership of a CXString and exposes it as std::string.
std::string take(CXString s) {
  const char *c = clang_getCString(s);
  std::string out = c ? c : "";
  clang_disposeString(s);
  return out;
}

constexpr std::array<std::string_view, 8> header_extensions{".h", ".hpp", ".hh",  ".hxx",
                                                            ".H", ".inl", ".ipp", ".tcc"};

bool looks_like_header(const std::filesystem::path &p) {
  const auto ext = p.extension().string();
  if (std::ranges::find(header_extensions, ext) != std::end(header_extensions))
    return true;
  // libstdc++-style extensionless headers under a c++/ directory.
  return ext.empty() && p.string().find("/c++/") != std::string::npos;
}

std::vector<std::filesystem::path> header_files(const std::filesystem::path &root) {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_regular_file(ec) && looks_like_header(it->path()))
      out.push_back(it->path());
  }
  std::ranges::sort(out);
  return out;
}

/// @brief Include directories offered to every parse: the root itself, each
///        first-level subdirectory (packages that expect `-I/usr/include/foo`
///        from pkg-config) and the multiarch directory.
std::vector<std::string> include_args(const std::filesystem::path &root) {
  std::vector<std::string> args{"-I" + root.string()};
  std::error_code ec;
  for (const auto &e : std::filesystem::directory_iterator(root, ec)) {
    if (e.is_directory(ec))
      args.push_back("-I" + e.path().string());
  }
  if (const auto ma = root / "x86_64-linux-gnu"; std::filesystem::is_directory(ma, ec)) {
    args.push_back("-I" + ma.string());
    for (const auto &e : std::filesystem::directory_iterator(ma, ec)) {
      if (e.is_directory(ec))
        args.push_back("-I" + e.path().string());
    }
  }
  return args;
}

// ----------------------------------------------------------------------------
// AST walk
// ----------------------------------------------------------------------------

constexpr std::array<CXCursorKind, 6> body_kinds{
  CXCursor_FunctionDecl, CXCursor_CXXMethod,  CXCursor_FunctionTemplate,
  CXCursor_Constructor,  CXCursor_Destructor, CXCursor_ConversionFunction,
};

/// @brief Decides the parse language for one header: C++-only extensions and
///        C++ constructs in the text force C++; otherwise the library default.
///        A text sniff, not a parse: it only picks which parser to run.
Language language_for(const std::filesystem::path &p, Language dflt) {
  const auto ext = p.extension().string();
  if (ext == ".hpp" || ext == ".hh" || ext == ".hxx" || ext == ".H" || ext == ".ipp" ||
      ext == ".tcc" || ext == ".inl") {
    return Language::cxx;
  }
  if (dflt == Language::cxx) {
    return Language::cxx;
  }
  const auto text = fs::read_file(p);
  if (!text) {
    return dflt;
  }
  static const std::regex cxx_marker(
    R"(^\s*(template\s*<|namespace\s+\w|class\s+\w+\s*[:{]|#include\s*<(string|vector|iostream|memory|functional)>))",
    std::regex::multiline
  );
  return std::regex_search(*text, cxx_marker) ? Language::cxx : dflt;
}

struct Walk {
  CXTranslationUnit tu;
  std::string root; ///< include_root as string, with trailing '/'
  HeaderIndex *out;
};

/// @brief Path of the file a cursor's expansion location is in; empty if none.
std::string cursor_file(CXCursor c) {
  CXFile file = nullptr;
  unsigned line = 0;
  unsigned col = 0;
  unsigned off = 0;
  clang_getExpansionLocation(clang_getCursorLocation(c), &file, &line, &col, &off);
  return file ? take(clang_getFileName(file)) : std::string{};
}

/// @brief Whitespace-normalised token spellings of a source range.
std::string tokens_of(CXTranslationUnit tu, CXSourceRange range, std::uint32_t *count) {
  CXToken *toks = nullptr;
  unsigned n = 0;
  clang_tokenize(tu, range, &toks, &n);
  std::string s;
  for (unsigned i = 0; i < n; ++i) {
    if (i)
      s.push_back(' ');
    s += take(clang_getTokenSpelling(tu, toks[i]));
  }
  clang_disposeTokens(tu, toks, n);
  if (count)
    *count = n;
  return s;
}

/// @brief Finds the CompoundStmt child of a definition cursor.
struct BodyFinder {
  CXCursor body;
  bool found;
};
CXChildVisitResult find_body(CXCursor c, CXCursor /*unused*/, CXClientData d) {
  if (clang_getCursorKind(c) == CXCursor_CompoundStmt) {
    auto *f = static_cast<BodyFinder *>(d);
    f->body = c;
    f->found = true;
    return CXChildVisit_Break;
  }
  return CXChildVisit_Continue;
}

std::string decl_signature(CXCursor c) {
  std::string s = take(clang_getCursorSpelling(c));
  s += '|';
  s += take(clang_getTypeSpelling(clang_getCursorResultType(c)));
  const int n = clang_Cursor_getNumArguments(c);
  for (int i = 0; i < n; ++i) {
    s += '|';
    s += take(clang_getTypeSpelling(
      clang_getCursorType(clang_Cursor_getArgument(c, static_cast<unsigned>(i)))
    ));
  }
  return s;
}

CXChildVisitResult visit(CXCursor c, CXCursor /*unused*/, CXClientData data) {
  auto *w = static_cast<Walk *>(data);
  const auto kind = clang_getCursorKind(c);

  // Only record entities declared in the package's own headers, but keep
  // descending: a namespace opened in a system header can contain them.
  const std::string file = std::filesystem::path(cursor_file(c)).lexically_normal().string();
  const bool own = file.starts_with(w->root);

  if (own && kind == CXCursor_MacroDefinition) {
    if (!clang_Cursor_isMacroFunctionLike(c)) {
      const std::string name = take(clang_getCursorSpelling(c));
      std::uint32_t n = 0;
      const std::string toks = tokens_of(w->tu, clang_getCursorExtent(c), &n);
      if (n > 1 && !name.starts_with('_')) { // has a value; not reserved
        const std::string rel = file.substr(w->root.size());
        w->out->macros[rel + "::" + name] = fingerprint(toks);
      }
    }
    return CXChildVisit_Continue;
  }

  if (own && std::ranges::find(body_kinds, kind) != std::end(body_kinds) &&
      clang_isCursorDefinition(c)) {
    BodyFinder bf{.body = clang_getNullCursor(), .found = false};
    clang_visitChildren(c, &find_body, &bf);
    if (bf.found) {
      std::uint32_t ntok = 0;
      const std::string body = tokens_of(w->tu, clang_getCursorExtent(bf.body), &ntok);
      std::string usr = take(clang_getCursorUSR(c));
      const std::string rel = file.substr(w->root.size());
      if (usr.empty())
        usr = rel + ":" + take(clang_getCursorSpelling(c));
      w->out->definitions[usr] = Definition{
        .relative_path = rel,
        .name = take(clang_getCursorSpelling(c)),
        .kind = take(clang_getCursorKindSpelling(kind)),
        .decl_fingerprint = fingerprint(decl_signature(c)),
        .body_fingerprint = fingerprint(body),
        .body_tokens = ntok
      };
    }
    // Do not descend into a function body: nested lambdas/local classes
    // are part of this body's fingerprint already.
    return CXChildVisit_Continue;
  }
  return CXChildVisit_Recurse;
}

} // namespace

// ----------------------------------------------------------------------------

Result<HeaderIndex> index(const std::filesystem::path &include_root, const Options &opt) {
  std::error_code ec;
  ABISTUDY_EXPECTS(std::filesystem::is_directory(include_root, ec));

  Index idx{clang_createIndex(/*excludeDeclarationsFromPCH=*/0, /*displayDiagnostics=*/0)};
  if (!idx)
    return fail(ErrorCode::clang, "clang_createIndex failed");

  HeaderIndex out;
  out.language = opt.language;
  // Everything -- header enumeration, -I flags and the ownership test on
  // cursor locations -- uses one canonical absolute root, so the paths clang
  // reports (which mirror what it was given) compare equal to it.
  const std::filesystem::path canon = std::filesystem::weakly_canonical(include_root, ec);
  std::string root = canon.string();
  if (!root.ends_with('/'))
    root.push_back('/');

  const std::vector<std::string> base_args = include_args(canon);
  auto args_for = [&](Language lang) {
    std::vector<std::string> args = base_args;
    args.emplace_back("-x");
    args.emplace_back(lang == Language::c ? "c" : "c++");
    args.emplace_back(lang == Language::c ? "-std=gnu11" : "-std=c++17");
    args.emplace_back("-ferror-limit=0");
    args.emplace_back("-w");
    args.emplace_back("-DNDEBUG");
    for (const auto &a : opt.extra_args) {
      args.push_back(a);
    }
    return args;
  };

  // KeepGoing is essential: without it a single missing #include makes the
  // whole translation unit empty, and DetailedPreprocessingRecord is what
  // exposes macro definitions as cursors.
  const unsigned flags =
    CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_KeepGoing;

  const auto files = header_files(canon);
  out.coverage.header_files = static_cast<std::uint32_t>(files.size());
  std::uint32_t done = 0;
  for (const auto &f : files) {
    if (done >= opt.max_files) {
      ++out.coverage.skipped_by_limit;
      continue;
    }
    ++done;
    const Language lang = language_for(f, opt.language);
    const auto args = args_for(lang);
    std::vector<const char *> argv;
    argv.reserve(args.size());
    for (const auto &a : args) {
      argv.push_back(a.c_str());
    }
    CXTranslationUnit raw = nullptr;
    const auto err = clang_parseTranslationUnit2(
      idx.get(), f.c_str(), argv.data(), static_cast<int>(argv.size()), nullptr, 0, flags, &raw
    );
    Tu tu{raw};
    if (err != CXError_Success || !tu) {
      continue;
    }
    ++out.coverage.parsed;
    if (lang == Language::cxx) {
      ++out.coverage.parsed_as_cxx;
    }
    bool fatal = false;
    bool error = false;
    const unsigned nd = clang_getNumDiagnostics(tu.get());
    for (unsigned i = 0; i < nd; ++i) {
      CXDiagnostic dg = clang_getDiagnostic(tu.get(), i);
      const auto sev = clang_getDiagnosticSeverity(dg);
      clang_disposeDiagnostic(dg);
      fatal |= sev == CXDiagnostic_Fatal;
      error |= sev >= CXDiagnostic_Error;
    }
    out.coverage.with_fatal_error += fatal ? 1 : 0;
    out.coverage.with_errors += error ? 1 : 0;
    Walk w{.tu = tu.get(), .root = root, .out = &out};
    clang_visitChildren(clang_getTranslationUnitCursor(tu.get()), &visit, &w);
  }
  return out;
}

/// @brief Version/build stamps change every release by construction.
bool looks_like_version_macro(std::string_view key) {
  static const std::regex stamp(
    "(VERSION|_DATE|BUILD|REVISION|RELEASE|PATCHLEVEL|COMMIT|GIT|SVN|HASH|_YEAR|_MONTH|TIMESTAMP|"
    "COPYRIGHT|MAJOR|MINOR|MICRO|_NUM$|_STR$)",
    std::regex::icase
  );
  const auto name = key.substr(key.rfind("::") == std::string_view::npos ? 0 : key.rfind("::") + 2);
  return std::regex_search(name.begin(), name.end(), stamp);
}

HeaderDiff compare(const HeaderIndex &a, const HeaderIndex &b) {
  HeaderDiff d;
  d.definitions_1 = static_cast<std::uint32_t>(a.definitions.size());
  d.definitions_2 = static_cast<std::uint32_t>(b.definitions.size());
  for (const auto &[usr, da] : a.definitions) {
    const auto it = b.definitions.find(usr);
    if (it == b.definitions.end()) {
      ++d.definitions_removed;
      continue;
    }
    ++d.definitions_common;
    const auto &db = it->second;
    if (da.body_fingerprint != db.body_fingerprint) {
      ++d.inline_body_changed;
      if (da.kind.find("Template") != std::string::npos)
        ++d.inline_body_changed_template;
      if (d.examples.size() < 8)
        d.examples.push_back(da.relative_path + "::" + da.name);
    }
    if (da.decl_fingerprint != db.decl_fingerprint)
      ++d.inline_decl_changed;
  }
  for (const auto &[usr, db] : b.definitions) {
    if (!a.definitions.contains(usr))
      ++d.definitions_added;
  }

  d.macros_1 = static_cast<std::uint32_t>(a.macros.size());
  d.macros_2 = static_cast<std::uint32_t>(b.macros.size());
  for (const auto &[k, va] : a.macros) {
    const auto it = b.macros.find(k);
    if (it == b.macros.end())
      continue;
    ++d.macros_common;
    if (it->second != va)
      ++d.macro_value_changed;
  }
  ABISTUDY_ENSURES(d.inline_body_changed <= d.definitions_common);
  ABISTUDY_ENSURES(d.macro_value_changed <= d.macros_common);
  return d;
}

// ----------------------------------------------------------------------------
// JSON
// ----------------------------------------------------------------------------

void to_json(nlohmann::json &j, const Definition &x) {
  j = {
    {"path", x.relative_path},
    {"name", x.name},
    {"kind", x.kind},
    {"decl", x.decl_fingerprint},
    {"body", x.body_fingerprint},
    {"tokens", x.body_tokens}
  };
}
void from_json(const nlohmann::json &j, Definition &x) {
  x.relative_path = j.at("path");
  x.name = j.at("name");
  x.kind = j.at("kind");
  x.decl_fingerprint = j.at("decl");
  x.body_fingerprint = j.at("body");
  x.body_tokens = j.at("tokens");
}
void to_json(nlohmann::json &j, const ParseCoverage &c) {
  j = {
    {"header_files", c.header_files},
    {"parsed", c.parsed},
    {"with_fatal_error", c.with_fatal_error},
    {"skipped_by_limit", c.skipped_by_limit}
  };
}
void from_json(const nlohmann::json &j, ParseCoverage &c) {
  c.header_files = j.at("header_files");
  c.parsed = j.at("parsed");
  c.with_fatal_error = j.at("with_fatal_error");
  c.with_errors = j.value("with_errors", 0U);
  c.parsed_as_cxx = j.value("parsed_as_cxx", 0U);
  c.skipped_by_limit = j.at("skipped_by_limit");
}
void to_json(nlohmann::json &j, const HeaderIndex &x) {
  j = {
    {"language", to_string(x.language)},
    {"definitions", x.definitions},
    {"macros", x.macros},
    {"coverage", x.coverage}
  };
}
void from_json(const nlohmann::json &j, HeaderIndex &x) {
  const auto l = j.at("language").get<std::string>();
  if (l == "c") {
    x.language = Language::c;
  } else if (l == "cxx") {
    x.language = Language::cxx;
  } else {
    x.language = Language::unknown;
  }
  x.definitions = j.at("definitions").get<std::unordered_map<std::string, Definition>>();
  x.macros = j.at("macros").get<std::map<std::string, std::string>>();
  x.coverage = j.at("coverage").get<ParseCoverage>();
}
void to_json(nlohmann::json &j, const HeaderDiff &d) {
  j = {
    {"definitions", {d.definitions_1, d.definitions_2}},
    {"definitions_common", d.definitions_common},
    {"inline_body_changed", d.inline_body_changed},
    {"inline_body_changed_template", d.inline_body_changed_template},
    {"inline_decl_changed", d.inline_decl_changed},
    {"definitions_added", d.definitions_added},
    {"definitions_removed", d.definitions_removed},
    {"macros", {d.macros_1, d.macros_2}},
    {"macros_common", d.macros_common},
    {"macro_value_changed", d.macro_value_changed},
    {"examples", d.examples}
  };
}
void from_json(const nlohmann::json &j, HeaderDiff &d) {
  d.definitions_1 = j.at("definitions")[0];
  d.definitions_2 = j.at("definitions")[1];
  d.definitions_common = j.at("definitions_common");
  d.inline_body_changed = j.at("inline_body_changed");
  d.inline_body_changed_template = j.at("inline_body_changed_template");
  d.inline_decl_changed = j.at("inline_decl_changed");
  d.definitions_added = j.at("definitions_added");
  d.definitions_removed = j.at("definitions_removed");
  d.macros_1 = j.at("macros")[0];
  d.macros_2 = j.at("macros")[1];
  d.macros_common = j.at("macros_common");
  d.macro_value_changed = j.at("macro_value_changed");
  d.macro_value_changed_nonversion =
    j.value("macro_value_changed_nonversion", d.macro_value_changed);
  d.examples = j.at("examples").get<std::vector<std::string>>();
}

} // namespace abistudy::hdr
