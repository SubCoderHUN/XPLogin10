// XPLogin10 - the XP sounds.
//
// PlaySound works on the Winlogon desktop: the audio service is running in
// session 0 and the logon session is allowed to render to the default device,
// which is how Windows plays its own logon chime. The sounds themselves are not
// shipped with this project - see NOTICE.md and tools/Import-XPSounds.ps1.
#include "Win32Common.h"

#include "xplogin/Config.h"
#include "xplogin/Interfaces.h"
#include "xplogin/Logging.h"
#include "xplogin/Win32Factories.h"

#include <mmsystem.h>
#include <shlwapi.h>

namespace xplogin {
namespace {

class Win32SoundPlayer : public ISoundPlayer {
public:
    explicit Win32SoundPlayer(SoundConfig config) : config_(std::move(config)) {}

    ~Win32SoundPlayer() override { StopAll(); }

    bool Play(SoundEvent event) override {
        if (!config_.enabled || config_.volumePercent == 0) {
            return false;
        }

        const std::wstring& path = PathFor(event);
        if (path.empty()) {
            return false;
        }
        if (!::PathFileExistsW(path.c_str())) {
            // A missing sound file is a cosmetic problem, never a logon problem.
            XPLOG_DEBUG("sound file missing, skipping");
            return false;
        }

        // Asynchronous so the UI thread keeps painting; NODEFAULT stops Windows
        // substituting its own beep when the file cannot be decoded.
        DWORD flags = SND_FILENAME | SND_ASYNC | SND_NODEFAULT;
        if (event == SoundEvent::Click || event == SoundEvent::Error) {
            // Short sounds must not cut off the logon chime.
            flags |= SND_NOSTOP;
        }

        if (!::PlaySoundW(path.c_str(), nullptr, flags)) {
            XPLOG_DEBUG("PlaySound failed for event %d", static_cast<int>(event));
            return false;
        }
        return true;
    }

    void StopAll() override { ::PlaySoundW(nullptr, nullptr, SND_PURGE); }

private:
    const std::wstring& PathFor(SoundEvent event) const {
        switch (event) {
            case SoundEvent::Logon: return config_.logonPath;
            case SoundEvent::Logoff: return config_.logoffPath;
            case SoundEvent::Error: return config_.errorPath;
            case SoundEvent::Click: return config_.clickPath;
            case SoundEvent::Shutdown: return config_.shutdownPath;
        }
        return empty_;
    }

    SoundConfig  config_;
    std::wstring empty_;
};

} // namespace

std::shared_ptr<ISoundPlayer> MakeWin32SoundPlayer(const SoundConfig& config) {
    return std::make_shared<Win32SoundPlayer>(config);
}

} // namespace xplogin
