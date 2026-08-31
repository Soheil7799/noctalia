#include "dbus/portal/file_chooser_portal.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "ui/dialogs/file_dialog.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("portal");

  constexpr auto kBusName = "org.freedesktop.impl.portal.desktop.noctalia";
  constexpr auto kObjectPath = "/org/freedesktop/portal/desktop";
  constexpr auto kInterface = "org.freedesktop.impl.portal.FileChooser";
  constexpr auto kRequestInterface = "org.freedesktop.impl.portal.Request";

  // Response codes from the portal spec: 0 accepted, 1 user cancelled,
  // 2 anything else.
  constexpr std::uint32_t kResponseSuccess = 0;
  constexpr std::uint32_t kResponseCancelled = 1;
  constexpr std::uint32_t kResponseError = 2;

  using Vardict = std::map<std::string, sdbus::Variant>;
  // Every FileChooser method has the same shape: (osssa{sv}) -> (ua{sv}).
  using CallResult = sdbus::Result<std::uint32_t, Vardict>;

  // filters is a(sa(us)): [(name, [(kind, pattern)])], kind 0 = glob, 1 = mime.
  using FilterPattern = sdbus::Struct<std::uint32_t, std::string>;
  using Filter = sdbus::Struct<std::string, std::vector<FilterPattern>>;

  template <typename T> std::optional<T> optionOf(const Vardict& options, std::string_view key) {
    const auto it = options.find(std::string{key});
    if (it == options.end() || !it->second.containsValueOfType<T>()) {
      return std::nullopt;
    }
    return it->second.get<T>();
  }

  /// current_folder and current_file arrive as `ay`: a NUL-terminated byte
  /// string, not a D-Bus string, because paths are not required to be UTF-8.
  std::optional<std::filesystem::path> pathFromBytes(const Vardict& options, std::string_view key) {
    const auto bytes = optionOf<std::vector<std::uint8_t>>(options, key);
    if (!bytes.has_value() || bytes->empty()) {
      return std::nullopt;
    }
    std::string text;
    text.reserve(bytes->size());
    for (const std::uint8_t byte : *bytes) {
      if (byte == 0) {
        break;
      }
      text.push_back(static_cast<char>(byte));
    }
    if (text.empty()) {
      return std::nullopt;
    }
    return std::filesystem::path{text};
  }

  /// Portal filters are globs ("*.png"); FileDialogOptions::extensions wants
  /// ".png" (see DirectoryScanner::imageExtensionFilter). Anything that is not a
  /// simple "*.ext" glob -- mime types, "*", "*.tar.*" -- is dropped rather than
  /// guessed at, because a wrong filter hides files the user asked for.
  std::vector<std::string> extensionsFromFilters(const Vardict& options) {
    std::vector<std::string> extensions;
    const auto filters = optionOf<std::vector<Filter>>(options, "filters");
    if (!filters.has_value()) {
      return extensions;
    }

    for (const auto& filter : *filters) {
      for (const auto& pattern : filter.get<1>()) {
        if (pattern.get<0>() != 0U) {
          continue; // mime type: nothing to match on a filename
        }
        const std::string& glob = pattern.get<1>();
        if (glob.size() < 3 || !glob.starts_with("*.")) {
          continue;
        }
        std::string ext = glob.substr(1); // "*.png" -> ".png"
        if (ext.find('*') != std::string::npos || ext.find('?') != std::string::npos) {
          continue;
        }
        std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (std::ranges::find(extensions, ext) == extensions.end()) {
          extensions.push_back(std::move(ext));
        }
      }
    }
    return extensions;
  }

  /// Backends must hand back normalized file:// URIs. Percent-encode everything
  /// outside RFC 3986 unreserved, keeping '/' so the path stays a path.
  std::string toFileUri(const std::filesystem::path& path) {
    static constexpr std::string_view kHex = "0123456789ABCDEF";
    const std::string text = path.string();
    std::string uri = "file://";
    uri.reserve(uri.size() + text.size());
    for (const unsigned char c : text) {
      const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'
                              || c == '_' || c == '.' || c == '~' || c == '/';
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

} // namespace

struct FileChooserPortal::Impl {
  explicit Impl(SessionBus& sessionBus) : bus(sessionBus) {}

  SessionBus& bus;
  std::unique_ptr<sdbus::IObject> object;
  /// The Request object the frontend calls Close() on. One at a time, because
  /// FileDialog is a single global dialog.
  std::unique_ptr<sdbus::IObject> request;
  std::optional<CallResult> pending;

  /// FileDialog is a process-wide singleton (see FileDialog::cancelIfPending),
  /// so a second concurrent portal call cannot be served. Report it honestly
  /// rather than cancelling the dialog the user is already looking at.
  [[nodiscard]] bool busy() const noexcept { return pending.has_value(); }

  void finish(std::uint32_t response, std::vector<std::string> uris) {
    if (!pending.has_value()) {
      return;
    }
    Vardict results;
    if (response == kResponseSuccess) {
      results.emplace("uris", sdbus::Variant{uris});
      // Open dialogs in this shell always yield a writable location; SaveFile
      // implies it too. Stated explicitly so callers do not have to assume.
      results.emplace("writable", sdbus::Variant{true});
    }
    auto result = std::move(*pending);
    pending.reset();
    request.reset();
    result.returnResults(response, results);
  }

  void exportRequest(const sdbus::ObjectPath& handle) {
    try {
      request = sdbus::createObject(bus.connection(), handle);
      request->addVTable(sdbus::registerMethod("Close").implementedAs([this]() {
        // The app gave up on us: drop the dialog and answer the pending call so
        // the frontend is not left waiting.
        FileDialog::cancelIfPending();
        finish(kResponseCancelled, {});
      })).forInterface(kRequestInterface);
    } catch (const sdbus::Error& e) {
      // Not fatal: without it, Close() from the app is ignored and the dialog
      // stays until the user answers it.
      kLog.warn("file chooser: could not export request object {}: {}", std::string{handle}, e.what());
      request.reset();
    }
  }

  /// Shared by all three methods: they differ only in how the options map onto
  /// FileDialogOptions and how the chosen path becomes the result URIs.
  void run(
      CallResult&& result, const sdbus::ObjectPath& handle, FileDialogOptions options,
      std::function<std::vector<std::string>(const std::filesystem::path&)> toUris
  ) {
    if (busy()) {
      kLog.warn("file chooser: a dialog is already open; refusing concurrent request");
      result.returnResults(kResponseError, Vardict{});
      return;
    }

    pending.emplace(std::move(result));
    exportRequest(handle);

    // Hop to the main loop: the dialog builds Wayland surfaces and scene nodes,
    // which must not happen inside a bus dispatch.
    DeferredCall::callLater([this, options = std::move(options), toUris = std::move(toUris)]() mutable {
      const bool opened =
          FileDialog::open(std::move(options), [this, toUris](std::optional<std::filesystem::path> picked) {
            if (!picked.has_value()) {
              finish(kResponseCancelled, {});
              return;
            }
            finish(kResponseSuccess, toUris(*picked));
          });
      if (!opened) {
        kLog.warn("file chooser: dialog refused to open");
        finish(kResponseError, {});
      }
    });
  }
};

FileChooserPortal::FileChooserPortal(SessionBus& bus) : m_impl(std::make_unique<Impl>(bus)) {
  auto* impl = m_impl.get();

  impl->object = sdbus::createObject(bus.connection(), sdbus::ObjectPath{kObjectPath});
  impl->object
      ->addVTable(
          sdbus::registerMethod("OpenFile")
              .implementedAs([impl](
                                 CallResult&& result, sdbus::ObjectPath handle, std::string /*appId*/,
                                 std::string /*parentWindow*/, std::string title, Vardict options
                             ) {
                FileDialogOptions dialog;
                // `directory` turns Open into a folder picker -- the same request
                // the wallpaper setting makes internally.
                const bool directory = optionOf<bool>(options, "directory").value_or(false);
                dialog.mode = directory ? FileDialogMode::SelectFolder : FileDialogMode::Open;
                dialog.title = std::move(title);
                if (!directory) {
                  dialog.extensions = extensionsFromFilters(options);
                }
                if (auto folder = pathFromBytes(options, "current_folder")) {
                  dialog.startDirectory = std::move(*folder);
                }
                // `multiple` is deliberately unhandled: the dialog is
                // single-selection (FileDialogView::m_selectedIndex) and its
                // callback yields one path. Returning a single URI is within
                // spec; widening it is a separate change to the view.
                impl->run(std::move(result), handle, std::move(dialog), [](const std::filesystem::path& picked) {
                  return std::vector<std::string>{toFileUri(picked)};
                });
              }),
          sdbus::registerMethod("SaveFile")
              .implementedAs([impl](
                                 CallResult&& result, sdbus::ObjectPath handle, std::string /*appId*/,
                                 std::string /*parentWindow*/, std::string title, Vardict options
                             ) {
                FileDialogOptions dialog;
                dialog.mode = FileDialogMode::Save;
                dialog.title = std::move(title);
                dialog.extensions = extensionsFromFilters(options);
                if (auto name = optionOf<std::string>(options, "current_name")) {
                  dialog.defaultFilename = std::move(*name);
                }
                if (auto folder = pathFromBytes(options, "current_folder")) {
                  dialog.startDirectory = std::move(*folder);
                }
                // current_file names an existing file being re-saved; it carries
                // both the directory and the name, so it wins over the two
                // separate hints when present.
                if (auto file = pathFromBytes(options, "current_file")) {
                  dialog.startDirectory = file->parent_path();
                  dialog.defaultFilename = file->filename().string();
                }
                impl->run(std::move(result), handle, std::move(dialog), [](const std::filesystem::path& picked) {
                  return std::vector<std::string>{toFileUri(picked)};
                });
              }),
          sdbus::registerMethod("SaveFiles")
              .implementedAs([impl](
                                 CallResult&& result, sdbus::ObjectPath handle, std::string /*appId*/,
                                 std::string /*parentWindow*/, std::string title, Vardict options
                             ) {
                // SaveFiles asks where to put a set of files the app already
                // named, so it is a folder pick plus a join -- not N dialogs.
                FileDialogOptions dialog;
                dialog.mode = FileDialogMode::SelectFolder;
                dialog.title = std::move(title);
                if (auto folder = pathFromBytes(options, "current_folder")) {
                  dialog.startDirectory = std::move(*folder);
                }

                std::vector<std::string> names;
                if (auto files = optionOf<std::vector<std::vector<std::uint8_t>>>(options, "files")) {
                  for (const auto& raw : *files) {
                    std::string name;
                    for (const std::uint8_t byte : raw) {
                      if (byte == 0) {
                        break;
                      }
                      name.push_back(static_cast<char>(byte));
                    }
                    // Only a bare filename may be joined; a path with separators
                    // could escape the directory the user chose.
                    if (!name.empty() && name.find('/') == std::string::npos && name != "." && name != "..") {
                      names.push_back(std::move(name));
                    }
                  }
                }

                impl->run(
                    std::move(result), handle, std::move(dialog),
                    [names = std::move(names)](const std::filesystem::path& folder) {
                      std::vector<std::string> uris;
                      uris.reserve(names.size());
                      for (const auto& name : names) {
                        uris.push_back(toFileUri(folder / name));
                      }
                      return uris;
                    }
                );
              })
      )
      .forInterface(kInterface);

  auto proxy = sdbus::createProxy(bus.connection(), sdbus::ServiceName{"org.freedesktop.DBus"},
                                  sdbus::ObjectPath{"/org/freedesktop/DBus"});
  std::uint32_t reply = 0;
  proxy->callMethod("RequestName")
      .onInterface("org.freedesktop.DBus")
      .withArguments(std::string{kBusName}, static_cast<std::uint32_t>(0))
      .storeResultsTo(reply);
  // 1 = primary owner, 4 = already owner. Anything else means another backend
  // holds the name and ours would never be called.
  if (reply != 1 && reply != 4) {
    throw std::runtime_error("could not acquire " + std::string{kBusName});
  }

  kLog.info("file chooser portal active on {}", kBusName);
}

FileChooserPortal::~FileChooserPortal() {
  if (m_impl != nullptr && m_impl->busy()) {
    FileDialog::cancelIfPending();
    m_impl->finish(kResponseError, {});
  }
}
