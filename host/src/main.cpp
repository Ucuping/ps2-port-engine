/*
 * Every game port's entry point. The game supplies its translated functions (the
 * table engine/tools/port.py generates) and its overrides; the rest is shared.
 */

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#else
#include <strings.h>
#include <unistd.h>
#endif

#include "platform/platform.h"
#include "ps2host/disc.h"
#include "ps2host/host.h"
#include "ps2host/run.h"
#include "ps2host/settings.h"

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace platform = ps2host::platform;

extern const ps2host::FunctionEntry kGuestFunctions[];
extern const std::size_t kGuestFunctionCount;
extern const ps2host::OverlayEntry kGuestOverlays[];
extern const std::size_t kGuestOverlayCount;
#ifdef PS2HOST_PGO_GENERATE
extern "C" int __llvm_profile_write_file(void);   // PS2HOST_PGO=generate (LLVM's profile runtime)
#endif

/* The game's host implementations of guest functions (its port's overrides.cpp). */
void RegisterGameOverrides();

namespace {

/* The port is built for AVX2 (engine/host/CMakeLists.txt; the VU's lanes and
   the rest take its three-operand forms): a processor without it is told so
   rather than stopped by an illegal instruction somewhere. */
bool HasAvx2() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)) && defined(__AVX2__)
    int info[4];
    __cpuid(info, 0);
    if (info[0] < 7) return false;
    __cpuid(info, 1);
    const bool osxsave = (info[2] >> 27) & 1, avx = (info[2] >> 28) & 1;
    if (!osxsave || !avx) return false;
    if ((_xgetbv(0) & 6) != 6) return false;        // the OS saves the YMM registers
    __cpuidex(info, 7, 0);
    return (info[1] >> 5) & 1;
#else
    return true;
#endif
}

std::string g_log;     // the log a run started without arguments writes

/* A message the player must see. On Windows a message box; elsewhere, for
   now, the terminal and the log (a dialog comes with the builder's app). */
void ShowMessage(const std::string& text, const char* title, bool error) {
#ifdef _WIN32
    MessageBoxA(nullptr, text.c_str(), title, MB_OK | (error ? MB_ICONERROR : MB_ICONWARNING));
#else
    std::fprintf(stderr, "[port] %s: %s\n", title, text.c_str());
    (void)error;
#endif
}

#ifdef PS2HOST_GAME_EXECUTABLE
bool SameIgnoringCase(const char* a, const char* b) {
#ifdef _WIN32
    return _stricmp(a, b) == 0;
#else
    return strcasecmp(a, b) == 0;
#endif
}

/* Whether `path` is an image of the game's disc: the executable it boots is
   the one the port was translated from (game.json's). */
bool IsGameDisc(const std::string& path) {
    ps2host::Disc disc;
    if (!disc.Open(path)) return false;
    std::string boot = disc.BootPath();
    const std::size_t semicolon = boot.find(';');
    if (semicolon != std::string::npos) boot.resize(semicolon);
    while (!boot.empty() && (boot[0] == '\\' || boot[0] == '/')) boot.erase(0, 1);
    return SameIgnoringCase(boot.c_str(), PS2HOST_GAME_EXECUTABLE);
}

/* The player's disc image, from a file dialog; "" if they cancel. Windows'
   dialog; elsewhere none yet - the image is given with --disc, or put beside
   the executable. */
std::string PickDisc() {
#ifndef _WIN32
    std::fprintf(stderr, "[port] no " PS2HOST_GAME_NAME " disc image: give one with --disc, or put it beside the "
                         "executable\n");
    return "";
#else
    char file[MAX_PATH] = {};
    OPENFILENAMEA dialog = {};
    dialog.lStructSize = sizeof dialog;
    dialog.lpstrFilter = "Disc images (*.iso; *.cue; *.bin)\0*.iso;*.cue;*.bin\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = "Choose your " PS2HOST_GAME_NAME " disc image";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&dialog) ? std::string(file) : std::string();
#endif
}
#endif

/* Started without arguments - from Explorer, a shortcut - the port plays its
   game from the player's own disc image: the one chosen before (kept in the
   PC settings, beside the executable), or asked for and checked to be the
   game's disc; the memory card a directory beside it too. In a window, with
   sound, on the GPU, with no time limit (the default is for test runs).
   An empty list: the player gave none. */
std::vector<std::string> PlayArguments(const char* self) {
#ifdef PS2HOST_GAME_EXECUTABLE
    const std::filesystem::path exe = platform::ExecutablePath();
    const std::filesystem::path here = exe.parent_path();
    ps2host::LoadSettings((here / "pc_settings.ini").string());     // where --memcard puts them
    std::string disc = ps2host::Setting("disc");
    // None chosen yet, or gone: an image of the game's disc beside the
    // executable is taken without asking.
    if (!IsGameDisc(disc)) {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(here, error)) {
            std::string ext = entry.path().extension().string();
            for (char& ch : ext) ch = char(tolower(static_cast<unsigned char>(ch)));
            if ((ext == ".iso" || ext == ".bin" || ext == ".cue") && IsGameDisc(entry.path().string())) {
                disc = entry.path().string();
                break;
            }
        }
    }
    while (!IsGameDisc(disc)) {
        std::error_code exists_error;
        if (!disc.empty() && std::filesystem::exists(disc, exists_error))
            ShowMessage("This is not an image of the " PS2HOST_GAME_NAME " disc "
                        "(" PS2HOST_GAME_EXECUTABLE "):\n\n" + disc, PS2HOST_GAME_NAME, false);
        disc = PickDisc();
        if (disc.empty()) return {};
    }
    if (disc != ps2host::Setting("disc")) ps2host::SetSetting("disc", disc);
    // No terminal to read: what the run says goes to a log beside it. Opened
    // once, shared (fopen's would lock the file to one stream), and given to
    // both: their lines in the order they were written.
    g_log = exe.stem().string() + ".log";
    const std::string log = (here / g_log).string();
#ifdef _WIN32
    std::FILE* stream = nullptr;
    freopen_s(&stream, "NUL", "w", stdout);    // streams with descriptors, in a windowed program
    freopen_s(&stream, "NUL", "w", stderr);
    const int file = _open(log.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_TEXT, _S_IREAD | _S_IWRITE);
    if (file >= 0) {
        _dup2(file, _fileno(stdout));
        _dup2(file, _fileno(stderr));
        _close(file);
    }
#else
    const int file = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file >= 0) {
        dup2(file, fileno(stdout));
        dup2(file, fileno(stderr));
        close(file);
    }
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a run ended by force loses nothing
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::fprintf(stderr, "[port] disc %s\n", disc.c_str());
    return {self, "--disc", disc, "--memcard", (here / "memcard").string(), "--window", "--gpu",
            "--audio", "--timeout", "315360000"};     // ten years
#else
    return {self};
#endif
}

}  // namespace

/* A port built as a windowed program has no console of its own. Started from
   a terminal its output goes to that one; redirected (a file, a pipe: the
   tools' runs), where it was sent. Windows' alone: elsewhere a program keeps
   the terminal it was started from. */
void AttachTerminal() {
#ifdef _WIN32
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    std::FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
#endif
}

#ifndef _WIN32
/* The run log for a run with arguments (one without writes its own,
   PlayArguments): beside the executable as <name>.log, or <name>.<pid>.log
   while another run holds that one; --log FILE names it. */
void StartLog(const std::string& asked, const char* self) {
    namespace fs = std::filesystem;
    const fs::path exe = platform::ExecutablePath().empty() ? fs::path(self) : platform::ExecutablePath();
    std::string path = asked.empty() ? (exe.parent_path() / (exe.stem().string() + ".log")).string() : asked;
    int started = platform::StartRunLog(path);
    if (started == 0 && asked.empty()) {
        path = (exe.parent_path() / (exe.stem().string() + "." + std::to_string(getpid()) + ".log")).string();
        started = platform::StartRunLog(path);
    }
    if (started != 1) {
        std::fprintf(stderr, "[log] cannot write the run log %s\n", path.c_str());
        return;
    }
    std::fprintf(stderr, "[log] %s\n", path.c_str());
}

/* Whatever ends the run says so in its log, besides the host's report and a
   crash's (platform.h): an exception nothing caught, exit() from a library, a
   signal from outside. */
void NoteOtherEnds() {
    platform::NoteEndingSignals();
    std::set_terminate([] {
        std::string what = "not a std::exception";
        if (const std::exception_ptr e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& x) {
                what = x.what();
            } catch (...) {
            }
        }
        std::fprintf(stderr, "[exit] an exception nothing caught (%s): the run aborts\n", what.c_str());
        std::abort();                       // the crash report follows (platform.h)
    });
    // The host leaves through std::_Exit below: exit() is someone else's.
    std::atexit([] {
        std::fprintf(stderr, "[exit] the process ends through exit() - not the host's own way out\n");
        platform::FinishRunLog();
    });
}
#endif

int main(int argc, char** argv) {
    AttachTerminal();
    std::vector<std::string> play;
    std::vector<char*> play_argv;
    if (argc == 1) {
        play = PlayArguments(argv[0]);
        if (play.empty()) return 0;
        for (std::string& arg : play) play_argv.push_back(arg.data());
        play_argv.push_back(nullptr);
        argc = int(play.size());
        argv = play_argv.data();
    }
#ifndef _WIN32
    // --log FILE, taken out before the host reads the rest.
    std::string log_path;
    std::vector<char*> rest;
    for (int i = 0; i < argc; ++i) {
        if (std::string(argv[i]) == "--log" && i + 1 < argc) {
            log_path = argv[++i];
            continue;
        }
        rest.push_back(argv[i]);
    }
    rest.push_back(nullptr);
    argc = int(rest.size()) - 1;
    argv = rest.data();
    NoteOtherEnds();
    if (play.empty()) StartLog(log_path, argv[0]);
#endif
    if (!HasAvx2()) {
        std::fprintf(stderr, "This build needs a processor with AVX2 (Intel Haswell, AMD Excavator or later).\n");
        return 70;
    }
#ifdef PS2HOST_GAME_TITLE
    ps2host::SetGameTitle(PS2HOST_GAME_TITLE);      // the window's (game.json's "title")
#endif
#ifdef PS2HOST_GAME_VERSION
    std::fprintf(stderr, "[port] %s, version %s\n", PS2HOST_GAME_TITLE, PS2HOST_GAME_VERSION);
#endif
    ps2host::RegisterFunctions(kGuestFunctions, kGuestFunctionCount);
    ps2host::RegisterOverlays(kGuestOverlays, kGuestOverlayCount);
    RegisterGameOverrides();
    const int code = ps2host::Main(argc, argv);
    if (!play.empty() && code != 0) {         // closing the window ends a run with 0
        std::fflush(nullptr);
        const std::string text = "The game stopped because of an error. What happened is in " +
                                 g_log + ", beside the game.";
        ShowMessage(text, ps2host::GameTitle().empty() ? "PS2 port" : ps2host::GameTitle().c_str(), true);
    }
    // What must be finished is (the report written, the settings and the
    // memory card saved as they changed, the WAV's header here); the rest -
    // the window, the GPU device, the threads - goes with the process.
    // Taken down one by one at exit, the Vulkan device's could hang
    // (vkDestroyDevice with the driver's threads and the renderer's images
    // still about), leaving a window that never closed.
    ps2host::CloseAudio();
    ps2host::ReleaseController();
    std::fflush(nullptr);
#ifdef PS2HOST_PGO_GENERATE
    __llvm_profile_write_file();            // (the profile's own writer runs at exit, which this skips)
#endif
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), UINT(code));
#else
    std::fprintf(stderr, "[exit] code %d\n", code);
    platform::FinishRunLog();
    std::_Exit(code);
#endif
    return code;
}
