#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

enum class FileDialogMode : std::uint8_t {
  Open,
  Save,
  SelectFolder,
};

enum class FileDialogViewMode : std::uint8_t {
  List,
  Grid,
};

struct FileDialogOptions {
  FileDialogMode mode = FileDialogMode::Open;
  FileDialogViewMode defaultViewMode = FileDialogViewMode::List;
  std::filesystem::path startDirectory;
  std::vector<std::string> extensions;
  std::string defaultFilename;
  std::string title;
  bool showHiddenFiles = false;
  /// Open mode only. Save picks one name and SelectFolder one directory, so
  /// neither has a meaning for a set.
  bool allowMultiple = false;

  FileDialogOptions& withHiddenFiles(bool show = true) {
    showHiddenFiles = show;
    return *this;
  }
};

class FileDialogPresenter {
public:
  virtual ~FileDialogPresenter() = default;

  [[nodiscard]] virtual bool openFileDialog() = 0;
  virtual void closeFileDialogWithoutResult() = 0;
};

class FileDialog {
public:
  using CompletionCallback = std::function<void(std::optional<std::filesystem::path>)>;
  using MultiCompletionCallback = std::function<void(std::vector<std::filesystem::path>)>;

  static void setPresenter(FileDialogPresenter* presenter) noexcept;
  [[nodiscard]] static bool open(FileDialogOptions options, CompletionCallback callback);
  /// Same dialog, but the caller wants every selected path. Sets allowMultiple
  /// on the options it is given. An empty vector means cancelled.
  ///
  /// A second entry point rather than a wider CompletionCallback: changing the
  /// existing signature would touch every call site in the shell to express
  /// something only the portal backend needs.
  [[nodiscard]] static bool openMultiple(FileDialogOptions options, MultiCompletionCallback callback);
  static void complete(std::optional<std::filesystem::path> result);
  static void completeMultiple(std::vector<std::filesystem::path> results);
  static void cancelIfPending();

  [[nodiscard]] static const FileDialogOptions& currentOptions();
};
