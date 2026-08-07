#include "FileExecutor.h"

#include <windows.h>

#include <sstream>

namespace xplogin::installer {
namespace {

bool DirectoryExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// True when the error means "there is nothing here", which is the only kind of
// failure an optional operation is allowed to shrug off.
bool IsAbsence(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
           error == ERROR_SUCCESS;
}

// The handful of codes worth naming. An uninstall that stops has to say why in
// words, because the number on its own sends people looking in the wrong place
// - 5 in particular reads as a permissions problem when it usually means the
// DLL is still loaded into LogonUI.
const wchar_t* DescribeError(DWORD error) {
    switch (error) {
        case ERROR_ACCESS_DENIED:      return L"access denied, or the file is still loaded";
        case ERROR_SHARING_VIOLATION:  return L"in use by another process";
        case ERROR_FILE_NOT_FOUND:     return L"no such file";
        case ERROR_PATH_NOT_FOUND:     return L"no such path";
        case ERROR_DIR_NOT_EMPTY:      return L"directory not empty";
        case ERROR_PRIVILEGE_NOT_HELD: return L"needs an elevated process";
        default:                       return L"see the Windows error code";
    }
}

} // namespace

bool FileExecutor::EnsureDirectory(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    if (DirectoryExists(path)) {
        return true;
    }

    // Walk down from the drive, creating each missing level. SHCreateDirectory
    // would do this too, but pulling in shell32 for one call is not worth it.
    size_t position = path.find(L'\\');
    while (position != std::wstring::npos) {
        const std::wstring level = path.substr(0, position);
        if (level.size() > 2 && !DirectoryExists(level)) {
            ::CreateDirectoryW(level.c_str(), nullptr);
        }
        position = path.find(L'\\', position + 1);
    }

    if (!::CreateDirectoryW(path.c_str(), nullptr)) {
        return ::GetLastError() == ERROR_ALREADY_EXISTS;
    }
    return true;
}

bool FileExecutor::DeleteFileOrScheduleForReboot(const std::wstring& path,
                                                 bool* scheduled) {
    if (scheduled) {
        *scheduled = false;
    }
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return true; // already gone
    }

    // Read-only files are ours to clear; we put them there.
    ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (::DeleteFileW(path.c_str())) {
        return true;
    }

    const DWORD error = ::GetLastError();
    if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) {
        return false;
    }

    // The uninstaller cannot delete the binary it is running from, so ask
    // Windows to do it during the next restart instead.
    if (::MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        if (scheduled) {
            *scheduled = true;
        }
        return true;
    }
    return false;
}

FileExecutionReport FileExecutor::Execute(const std::vector<FileOperation>& plan) {
    FileExecutionReport report;
    std::wostringstream transcript;

    for (const FileOperation& op : plan) {
        bool ok = false;
        std::wstring note;

        switch (op.kind) {
            case FileOpKind::MakeDirectory:
                ok = EnsureDirectory(op.target);
                break;

            case FileOpKind::Copy: {
                // A missing optional payload file is not a failure.
                if (::GetFileAttributesW(op.source.c_str()) ==
                    INVALID_FILE_ATTRIBUTES) {
                    ok = op.optional;
                    note = L"source not present";
                    break;
                }
                // Overwrite: reinstalling over an existing copy is normal.
                ok = ::CopyFileW(op.source.c_str(), op.target.c_str(), FALSE) != FALSE;
                const DWORD copyError = ok ? ERROR_SUCCESS : ::GetLastError();
                if (!ok && (copyError == ERROR_ACCESS_DENIED ||
                            copyError == ERROR_SHARING_VIOLATION)) {
                    // A file in use can still be replaced after a restart.
                    // Both codes mean that: a running .exe reports 32
                    // (sharing violation) and a loaded .dll reports 5, and
                    // only handling one of them is how re-installing over a
                    // running watchdog failed outright.
                    std::wstring staged = op.target + L".new";
                    if (::CopyFileW(op.source.c_str(), staged.c_str(), FALSE) &&
                        ::MoveFileExW(staged.c_str(), op.target.c_str(),
                                      MOVEFILE_DELAY_UNTIL_REBOOT |
                                          MOVEFILE_REPLACE_EXISTING)) {
                        ok = true;
                        report.rebootRequired = true;
                        note = L"in use, replaced on restart";
                    }
                }
                break;
            }

            case FileOpKind::Delete: {
                bool scheduled = false;
                ok = DeleteFileOrScheduleForReboot(op.target, &scheduled);
                if (scheduled) {
                    report.rebootRequired = true;
                    note = L"in use, removed on restart";
                }
                break;
            }

            case FileOpKind::RemoveEmptyDirectory:
                // Deliberately not recursive: anything the user put in the
                // folder is theirs, and an empty-directory failure here is a
                // better outcome than deleting it.
                ok = ::RemoveDirectoryW(op.target.c_str()) != FALSE;
                if (!ok && ::GetLastError() == ERROR_DIR_NOT_EMPTY) {
                    ::MoveFileExW(op.target.c_str(), nullptr,
                                  MOVEFILE_DELAY_UNTIL_REBOOT);
                    ok = true;
                    report.rebootRequired = true;
                    note = L"not empty, removed on restart";
                }
                break;
        }

        const DWORD failure = ok ? ERROR_SUCCESS : ::GetLastError();

        // `optional` means "this target may legitimately not be there" - a
        // payload file the build did not produce, a registry-era leftover, a
        // second uninstall. It does not mean "any error is acceptable".
        //
        // It used to mean the second thing, and that is how an uninstall could
        // delete nothing at all and still report success: every removal in the
        // plan is optional, so an access denied or a sharing violation on the
        // very first file was rewritten to "ok (skipped)" and the run finished
        // with a clean transcript over an untouched directory.
        if (!ok && op.optional && IsAbsence(failure)) {
            ok = true;
            if (note.empty()) {
                note = L"not present";
            }
        }

        transcript << (ok ? L"  ok   " : L"  FAIL ") << op.target;
        if (!note.empty()) {
            transcript << L"  (" << note << L")";
        } else if (!ok) {
            transcript << L"  (error " << failure << L": " << DescribeError(failure)
                       << L")";
        }
        transcript << L"\r\n";

        if (!ok) {
            report.success = false;
            report.transcript = transcript.str();
            return report;
        }
        ++report.completed;
    }

    report.transcript = transcript.str();
    return report;
}

} // namespace xplogin::installer
