#include "dbus/portal/file_chooser_util.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

  bool check(bool cond, const char* msg) {
    if (!cond) {
      std::cerr << "FAIL: " << msg << '\n';
    }
    return cond;
  }

  using file_chooser_util::extensionFromGlob;
  using file_chooser_util::extensionsFromGlobs;
  using file_chooser_util::stateKeyForApp;
  using file_chooser_util::stripMnemonics;
  using file_chooser_util::toFileUri;

  bool isExt(std::string_view glob, std::string_view expected) {
    const auto ext = extensionFromGlob(glob);
    return ext.has_value() && *ext == expected;
  }

  bool rejects(std::string_view glob) { return !extensionFromGlob(glob).has_value(); }

} // namespace

int main() {
  bool ok = true;

  // -- plain globs ---------------------------------------------------------
  ok &= check(isExt("*.png", ".png"), "plain glob");
  ok &= check(isExt("*.PNG", ".png"), "uppercase glob folds to lower");
  ok &= check(isExt("*.tar.gz", ".tar.gz"), "multi-dot suffix kept whole");

  // -- character classes ---------------------------------------------------
  // The case this exists for: Chromium-family browsers spell case-insensitivity
  // as classes, and reading them literally produced ".[pp][nn][gg]", which
  // matched nothing -- the dialog showed an empty directory of images.
  ok &= check(isExt("*.[pP][nN][gG]", ".png"), "case-variant classes collapse");
  ok &= check(isExt("*.jp[gG]", ".jpg"), "class mixed with literals");
  ok &= check(isExt("*.[p][n][g]", ".png"), "single-member classes");

  // A class whose members are genuinely different characters is a real
  // alternation, which one extension cannot represent. Reducing it to its first
  // member would silently filter to the wrong set.
  ok &= check(rejects("*.[0-9]"), "numeric range rejected");
  ok &= check(rejects("*.[abc]"), "true alternation rejected");
  ok &= check(rejects("*.[pP][0-9]"), "one bad class poisons an otherwise reducible glob");

  // -- malformed / wildcard ------------------------------------------------
  ok &= check(rejects("*"), "bare star");
  ok &= check(rejects("*.*"), "star suffix");
  ok &= check(rejects("*."), "empty suffix");
  ok &= check(rejects("*.p?g"), "question mark");
  ok &= check(rejects("*.pn*"), "embedded star");
  ok &= check(rejects("*.[png"), "unterminated class");
  ok &= check(rejects("*.[]"), "empty class");
  ok &= check(rejects("photo.png"), "not a glob");
  ok &= check(rejects(""), "empty input");

  // -- merging filters -----------------------------------------------------
  {
    const std::vector<std::string> globs{"*.png", "*.jpg"};
    const auto exts = extensionsFromGlobs(globs);
    ok &= check(exts.size() == 2 && exts[0] == ".png" && exts[1] == ".jpg", "merges distinct globs in order");
  }
  {
    // Same extension reached two ways must not be listed twice.
    const std::vector<std::string> globs{"*.png", "*.PNG", "*.[pP][nN][gG]"};
    const auto exts = extensionsFromGlobs(globs);
    ok &= check(exts.size() == 1 && exts[0] == ".png", "deduplicates after folding");
  }
  {
    // A catch-all anywhere disables filtering: if the app offers "All Files",
    // hiding files the user is entitled to pick would be wrong.
    const std::vector<std::string> globs{"*.png", "*.*"};
    ok &= check(extensionsFromGlobs(globs).empty(), "catch-all disables filtering");
  }
  {
    const std::vector<std::string> globs{"*", "*.png"};
    ok &= check(extensionsFromGlobs(globs).empty(), "bare-star catch-all disables filtering");
  }
  {
    ok &= check(extensionsFromGlobs({}).empty(), "no filters means no filtering");
  }
  {
    // Nothing reducible: the result is empty, which reads as "show everything".
    // Showing too much is the safe failure; showing nothing would strand the user
    // in a directory that looks empty.
    const std::vector<std::string> globs{"*.[0-9]", "*.p?g"};
    ok &= check(extensionsFromGlobs(globs).empty(), "unreducible globs fall back to no filtering");
  }

  // -- file:// URIs --------------------------------------------------------
  ok &= check(toFileUri("/home/user/a.png") == "file:///home/user/a.png", "plain path");
  ok &= check(toFileUri("/home/user/my file.png") == "file:///home/user/my%20file.png", "space encoded");
  ok &= check(toFileUri("/tmp/a-b_c.d~e") == "file:///tmp/a-b_c.d~e", "unreserved characters kept");
  ok &= check(toFileUri("/tmp/100%.txt") == "file:///tmp/100%25.txt", "percent itself encoded");
  ok &= check(toFileUri("/tmp/a#b?c.txt") == "file:///tmp/a%23b%3Fc.txt", "fragment and query delimiters encoded");
  // Non-ASCII is encoded per UTF-8 byte, not per character.
  ok &= check(toFileUri("/tmp/café.txt") == "file:///tmp/caf%C3%A9.txt", "utf-8 encoded bytewise");

  // -- accept_label mnemonics ----------------------------------------------
  ok &= check(stripMnemonics("_Open") == "Open", "leading accelerator dropped");
  ok &= check(stripMnemonics("Se_lect") == "Select", "inner accelerator dropped");
  ok &= check(stripMnemonics("Save __as") == "Save _as", "doubled underscore is literal");
  ok &= check(stripMnemonics("Upload") == "Upload", "plain label untouched");
  ok &= check(stripMnemonics("") == "", "empty label");
  ok &= check(stripMnemonics("_") == "", "trailing marker with nothing to mark");
  ok &= check(stripMnemonics("__") == "_", "escaped underscore alone");

  // -- state keys for app ids ----------------------------------------------
  ok &= check(stateKeyForApp("org.mozilla.firefox") == "org.mozilla.firefox", "well-formed id kept");
  ok &= check(stateKeyForApp("my app/v2") == "my_app_v2", "spaces and slashes folded");
  ok &= check(stateKeyForApp("") == "default", "empty id shares one memory");
  ok &= check(stateKeyForApp("...") == "default", "separator-only id is not an identity");
  ok &= check(stateKeyForApp("a\nb") == "a_b", "newline cannot break the table name");

  return ok ? 0 : 1;
}
