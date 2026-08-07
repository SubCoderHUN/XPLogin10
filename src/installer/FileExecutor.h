// XPLogin10 - applies a validated DeploymentPlan to the real file system.
#pragma once

#include "xplogin/DeploymentPlan.h"

#include <string>

namespace xplogin::installer {

struct FileExecutionReport {
    bool         success = true;
    size_t       completed = 0;
    std::wstring transcript;
    // True when something could only be removed on the next restart - the
    // running uninstaller cannot delete itself.
    bool         rebootRequired = false;
};

class FileExecutor {
public:
    // Stops at the first real failure.
    //
    // `optional` on an operation means the target may legitimately not be
    // there - a payload file this build did not produce, a leftover from an
    // older version, a second uninstall over an already-clean directory. It
    // does NOT mean the operation may fail for any reason: an access denied or
    // a sharing violation on an optional target still stops the run and still
    // says so. Treating those as "skipped" is how an uninstall used to remove
    // nothing and report success.
    FileExecutionReport Execute(const std::vector<FileOperation>& plan);

    // Creates every missing level of `path`.
    static bool EnsureDirectory(const std::wstring& path);

    // Deletes a file, falling back to a delete-on-reboot request when it is
    // locked (which is exactly what happens to the running setup binary).
    static bool DeleteFileOrScheduleForReboot(const std::wstring& path,
                                              bool* scheduled);
};

} // namespace xplogin::installer
