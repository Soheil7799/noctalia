#include "dbus/portal/file_chooser_util.h"

#include <algorithm>
#include <cctype>

namespace file_chooser_util {

  std::optional<std::string> extensionFromGlob(std::string_view glob) {
    if (!glob.starts_with("*.")) {
      return std::nullopt;
    }
    const std::string_view rest = glob.substr(2);
    if (rest.empty()) {
      return std::nullopt;
    }

    std::string ext = ".";
    for (std::size_t i = 0; i < rest.size();) {
      const char c = rest[i];
      if (c == '*' || c == '?') {
        return std::nullopt; // still a wildcard: not a fixed extension
      }
      if (c == '[') {
        const std::size_t close = rest.find(']', i + 1);
        if (close == std::string_view::npos) {
          return std::nullopt;
        }
        const std::string_view cls = rest.substr(i + 1, close - i - 1);
        if (cls.empty()) {
          return std::nullopt;
        }
        // Only a case-variant class collapses to one character. Anything else
        // ([0-9], [abc]) is a genuine alternation this cannot represent.
        const char first = static_cast<char>(std::tolower(static_cast<unsigned char>(cls.front())));
        for (const char member : cls) {
          if (static_cast<char>(std::tolower(static_cast<unsigned char>(member))) != first) {
            return std::nullopt;
          }
        }
        ext.push_back(first);
        i = close + 1;
        continue;
      }
      ext.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      ++i;
    }
    return ext;
  }

  std::vector<std::string> extensionsFromGlobs(std::span<const std::string> globs) {
    std::vector<std::string> extensions;
    for (const auto& glob : globs) {
      if (glob == "*" || glob == "*.*") {
        return {}; // catch-all present: show everything
      }
      if (auto ext = extensionFromGlob(glob)) {
        if (std::ranges::find(extensions, *ext) == extensions.end()) {
          extensions.push_back(std::move(*ext));
        }
      }
    }
    return extensions;
  }

  std::string stripMnemonics(std::string_view label) {
    std::string out;
    out.reserve(label.size());
    for (std::size_t i = 0; i < label.size(); ++i) {
      if (label[i] != '_') {
        out.push_back(label[i]);
        continue;
      }
      if (i + 1 < label.size() && label[i + 1] == '_') {
        out.push_back('_'); // escaped: one literal underscore
        ++i;
      }
      // else: accelerator marker, dropped
    }
    return out;
  }

  std::string toFileUri(const std::filesystem::path& path) {
    static constexpr std::string_view kHex = "0123456789ABCDEF";
    const std::string text = path.string();
    std::string uri = "file://";
    uri.reserve(uri.size() + text.size());
    for (const unsigned char c : text) {
      const bool unreserved = (c >= 'A' && c <= 'Z')
          || (c >= 'a' && c <= 'z')
          || (c >= '0' && c <= '9')
          || c == '-'
          || c == '_'
          || c == '.'
          || c == '~'
          || c == '/';
      if (unreserved) {
        uri.push_back(static_cast<char>(c));
      } else {
        uri.push_back('%');
        uri.push_back(kHex[c >> 4U]);
        uri.push_back(kHex[c & 0x0FU]);
      }
    }
    return uri;
  }

} // namespace file_chooser_util
