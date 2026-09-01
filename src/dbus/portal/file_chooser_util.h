#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Pure helpers behind the FileChooser portal backend: turning the filter globs
/// an application sends into the extension list the dialog understands, and
/// turning a chosen path back into the file:// URI the portal must return.
///
/// Kept out of file_chooser_portal.cpp so they can be exercised without a bus.
namespace file_chooser_util {

  /// A glob reduced to the ".ext" form DirectoryScanner expects, or nullopt when
  /// it cannot be expressed as one.
  ///
  /// Real-world filters are not literal: browsers spell case-insensitivity as
  /// character classes, so Brave asks for `*.[pP][nN][gG]` rather than `*.png`.
  /// Treating that as literal text yields ".[pp][nn][gg]", which matches no file
  /// on earth -- the dialog then opens on a directory of images and reports that
  /// none of them match.
  [[nodiscard]] std::optional<std::string> extensionFromGlob(std::string_view glob);

  /// Filter globs -> FileDialogOptions::extensions.
  ///
  /// The dialog has no filter dropdown, so every filter's patterns are merged.
  /// That makes a catch-all decisive: browsers send an "All Files" entry of `*.*`
  /// alongside the specific types, and if the user is offered "all files" at all,
  /// filtering to a subset would hide files they are entitled to pick. So one
  /// catch-all disables filtering entirely.
  ///
  /// An empty result means "no filtering", which is also what a set of globs none
  /// of which can be reduced produces -- showing everything is the safe failure.
  [[nodiscard]] std::vector<std::string> extensionsFromGlobs(std::span<const std::string> globs);

  /// An accept_label with GTK mnemonics resolved: a single underscore marks the
  /// following character as an accelerator and is dropped, a doubled one is a
  /// literal underscore. Portal clients send "_Open" or "Se_lect"; rendering that
  /// verbatim puts a stray underscore on the button.
  [[nodiscard]] std::string stripMnemonics(std::string_view label);

  /// Backends must hand back normalized file:// URIs. Percent-encodes everything
  /// outside RFC 3986 unreserved, keeping '/' so the path stays a path.
  [[nodiscard]] std::string toFileUri(const std::filesystem::path& path);

} // namespace file_chooser_util
