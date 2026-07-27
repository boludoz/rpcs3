// rpcs3 headers are not self-contained: every TU in the tree includes
// stdafx.h first (std includes + rpcs3's fmt namespace + [[noreturn]] decls).
#include "stdafx.h"

#include "Crypto/unpkg.h"
#include "Crypto/unself.h"
#include "Emu/Audio/Cubeb/CubebBackend.h"
#include "Emu/Audio/Null/NullAudioBackend.h"
#include "Emu/Cell/PPUAnalyser.h"
#include "Emu/Cell/SPURecompiler.h"
#include "Emu/IdManager.h"
#include "Emu/Io/KeyboardHandler.h"
#include "Emu/Io/Null/NullKeyboardHandler.h"
#include "Emu/Io/Null/NullMouseHandler.h"
#include "Emu/Io/Null/NullPadHandler.h"
#include "Emu/Io/Null/null_camera_handler.h"
#include "Emu/Io/Null/null_music_handler.h"
#include "Emu/Io/pad_config_types.h"
#include "Emu/RSX/Null/NullGSRender.h"
#include "Emu/RSX/Overlays/overlay_manager.h"
#include "Emu/RSX/Overlays/overlay_save_dialog.h"
#include "Emu/RSX/Overlays/overlay_trophy_notification.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/VK/VKGSRender.h"
#include "Emu/localized_string_id.h"
#include "Emu/system_config.h"
#include "Emu/system_config_types.h"
#include "Emu/system_progress.hpp"
#include "Emu/system_utils.hpp"
#include "Emu/vfs_config.h"
#include "Input/ds3_pad_handler.h"
#include "Input/ds4_pad_handler.h"
#include "Input/dualsense_pad_handler.h"
#include "Input/hid_pad_handler.h"
#include "Input/pad_thread.h"
#include "Input/virtual_pad_handler.h"
#include "Loader/ISO.h"
#include "Loader/PSF.h"
#include "Loader/PUP.h"
#include "Loader/TAR.h"
#include "Emu/Cell/lv2/sys_sync.h"
#include "hidapi_libusb.h"
#include "libusb.h"
#include "rpcs3_version.h"
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/Cell/Modules/cellSysutil.h"
#include "Utilities/File.h"
#include "Utilities/JIT.h"
#include "util/asm.hpp"
#include "Utilities/StrFmt.h"
#include "Utilities/StrUtil.h"
#include "Utilities/Thread.h"
#include "util/console.h"
#include "util/fixed_typemap.hpp"
#include "util/logs.hpp"
#include "util/serialization.hpp"
#include "util/sysinfo.hpp"
#include "util/yaml.hpp"
#include <Emu/Io/pad_config.h>
#include <Emu/RSX/GSFrameBase.h>
#include <Emu/System.h>
#include "Emu/Cell/Modules/cellSaveData.h"
#include "Emu/Cell/Modules/sceNpTrophy.h"

#include <algorithm>
#include <cctype>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <iterator>
#include <jni.h>
#include <optional>
#include <span>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type-c-linkage"

struct AtExit {
  std::function<void()> cb;
  ~AtExit() { cb(); }
};

static bool g_initialized;
static std::atomic<ANativeWindow *> g_native_window;

extern std::string g_android_executable_dir;
extern std::string g_android_config_dir;
extern std::string g_android_cache_dir;

// Player slots wired to Android-side virtual pads (touch overlay or
// physical controllers relayed through the Kotlin input layer). RPCS3's
// pad_thread supports up to 7 players (g_cfg_input.player1..player7); we
// only configure the first kMaxVirtualPads of them as virtual_pad devices.
static constexpr int kMaxVirtualPads = 4;
static std::mutex g_virtual_pad_mutex;
static std::shared_ptr<Pad> g_virtual_pads[kMaxVirtualPads];

std::string g_input_config_override;
cfg_input_configurations g_cfg_input_configs;

LOG_CHANNEL(rpcsx_android, "ANDROID");

// Defined in the settings bridge below: persists the live g_cfg to the
// global config.json as JSON.
static void save_global_config();

struct LogListener : logs::listener {
  LogListener() { logs::listener::add(this); }

  void log(u64 stamp, const logs::message &msg, std::string_view prefix,
           std::string_view text) override {
    int prio = 0;
    switch (static_cast<logs::level>(msg)) {
    case logs::level::always:
      prio = ANDROID_LOG_INFO;
      break;
    case logs::level::fatal:
      prio = ANDROID_LOG_FATAL;
      break;
    case logs::level::error:
      prio = ANDROID_LOG_ERROR;
      break;
    case logs::level::todo:
      prio = ANDROID_LOG_WARN;
      break;
    case logs::level::success:
      prio = ANDROID_LOG_INFO;
      break;
    case logs::level::warning:
      prio = ANDROID_LOG_WARN;
      break;
    case logs::level::notice:
      prio = ANDROID_LOG_DEBUG;
      break;
    case logs::level::trace:
      prio = ANDROID_LOG_VERBOSE;
      break;
    }

    __android_log_write(prio, "RPCS3", std::string(text).c_str());
  }
} static g_androidLogListener;

struct GraphicsFrame : GSFrameBase {
  mutable ANativeWindow *activeNativeWindow = nullptr;
  mutable int width = 0;
  mutable int height = 0;

  ~GraphicsFrame() {
    if (activeNativeWindow != nullptr) {
      ANativeWindow_release(activeNativeWindow);
    }
  }

  ANativeWindow *getNativeWindow() const {
    ANativeWindow *result;
    while ((result = g_native_window.load()) == nullptr) [[unlikely]] {
      if (Emu.IsStopped()) {
        return activeNativeWindow;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (result != activeNativeWindow) [[unlikely]] {
      ANativeWindow_acquire(result);

      if (activeNativeWindow != nullptr) {
        ANativeWindow_release(activeNativeWindow);
      }

      activeNativeWindow = result;
    }

    if (result != nullptr) {
      width = ANativeWindow_getWidth(result);
      height = ANativeWindow_getHeight(result);
    }

    return result;
  }

  void close() override {}
  void reset() override {}
  bool shown() override { return true; }
  void hide() override {}
  void show() override {}
  void toggle_fullscreen() override {}

  void delete_context(draw_context_t ctx) override {}
  draw_context_t make_context() override { return nullptr; }
  void set_current(draw_context_t ctx) override {}
  void flip(draw_context_t ctx, bool skip_frame = false) override {}
  int client_width() override {
    if (auto win = getNativeWindow()) {
      width = ANativeWindow_getWidth(win);
    }
    return width;
  }
  int client_height() override {
    if (auto win = getNativeWindow()) {
      height = ANativeWindow_getHeight(win);
    }
    return height;
  }
  f64 client_display_rate() override { return 120.0f; }
  bool has_alpha() override {
    return ANativeWindow_getFormat(getNativeWindow()) ==
           WINDOW_FORMAT_RGBA_8888;
  }

  display_handle_t handle() const override { return getNativeWindow(); }

  bool can_consume_frame() const override { return false; }

  void present_frame(std::vector<u8>&& data, u32 pitch, u32 width, u32 height,
                     bool is_bgra) const override {}
  void update_title(double fps = 0.0) override {}
  void take_screenshot(std::vector<u8> &&sshot_data, u32 sshot_width,
                       u32 sshot_height, bool is_bgra) override {}
};

void jit_announce(uptr, usz, std::string_view);

[[noreturn]] void report_fatal_error(std::string_view _text,
                                     bool is_html = false,
                                     bool include_help_text = true) {
  std::string buf;

  buf = std::string(_text);

  // Check if thread id is in string
  if (_text.find("\nThread id = "sv) == umax && !thread_ctrl::is_main()) {
    // Append thread id if it isn't already, except on main thread
    fmt::append(buf, "\n\nThread id = %u.", thread_ctrl::get_tid());
  }

  if (!g_tls_serialize_name.empty()) {
    fmt::append(buf, "\nSerialized Object: %s", g_tls_serialize_name);
  }

  const system_state state = Emu.GetStatus(false);

  if (state == system_state::stopped) {
    fmt::append(buf, "\nEmulation is stopped");
  } else {
    const std::string &name = Emu.GetTitleAndTitleID();
    fmt::append(buf, "\nTitle: \"%s\" (emulation is %s)",
                name.empty() ? "N/A" : name.data(),
                state == system_state::stopping ? "stopping" : "running");
  }

  fmt::append(buf, "\nBuild: \"%s\"", rpcs3::get_verbose_version());
  fmt::append(buf, "\nDate: \"%s\"", std::chrono::system_clock::now());

  __android_log_write(ANDROID_LOG_FATAL, "RPCS3", buf.c_str());

  jit_announce(0, 0, "");
  utils::trap();
  std::abort();
  std::terminate();
}

void qt_events_aware_op(int repeat_duration_ms,
                        std::function<bool()> wrapped_op) {
  /// ?????
}

static std::string unwrap(JNIEnv *env, jstring string) {
  auto resultBuffer = env->GetStringUTFChars(string, nullptr);
  std::string result(resultBuffer);
  env->ReleaseStringUTFChars(string, resultBuffer);
  return result;
}
static jstring wrap(JNIEnv *env, const std::string &string) {
  return env->NewStringUTF(string.c_str());
}
static jstring wrap(JNIEnv *env, const char *string) {
  return env->NewStringUTF(string);
}

static std::string resolveTreeUriToPath(JNIEnv *env, std::string_view uri);

static std::string fix_dir_path(std::string string) {
  if (!string.empty() && !string.ends_with('/')) {
    string += '/';
  }

  return string;
}

enum class FileType {
  Unknown,
  Pup,
  Pkg,
  Edat,
  Rap,
  Iso,
};

static bool is_iso_file_local(const fs::file& file) {
  if (!file || file.size() < 32768ULL + 6) {
    return false;
  }
  char magic[5];
  file.read_at(32768ULL + 1, magic, 5);
  return magic[0] == 'C' && magic[1] == 'D' && magic[2] == '0' && magic[3] == '0' && magic[4] == '1';
}

static FileType getFileType(const fs::file &file) {
  file.seek(0);
  if (PUPHeader pupHeader; file.read(pupHeader)) {
    if (pupHeader.magic == "SCEUF\0\0\0"_u64) {
      return FileType::Pup;
    }
  }

  file.seek(0);
  if (PKGHeader pkgHeader; file.read(pkgHeader)) {
    if (pkgHeader.pkg_magic == std::bit_cast<le_t<u32>>("\x7FPKG"_u32)) {
      return FileType::Pkg;
    }
  }

  file.seek(0);
  if (NPD_HEADER npdHeader; file.read(npdHeader)) {
    if (npdHeader.magic == "NPD\0"_u32) {
      return FileType::Edat;
    }
  }

  if (file.size() == 16) {
    return FileType::Rap;
  }

  // Check ISO header directly on duplicated handle to avoid procfs SELinux restrictions
  if (file.get_handle() >= 0) {
    fs::file dupFile = fs::file::from_native_handle(dup(file.get_handle()));
    if (is_iso_file_local(dupFile)) {
      return FileType::Iso;
    }
  }

  rpcsx_android.notice("getFileType: not an ISO (fd=%d, size=%llu)", file.get_handle(),
                       file.size());
  return FileType::Unknown;
}

#define MAKE_STRING(id, x) [int(localized_string_id::id)] = {x, U##x}

static std::pair<std::string, std::u32string> g_strings[] = {
    MAKE_STRING(RSX_OVERLAYS_COMPILING_SHADERS, "Compiling shaders"),
    MAKE_STRING(RSX_OVERLAYS_COMPILING_PPU_MODULES, "Compiling PPU Modules"),
    MAKE_STRING(RSX_OVERLAYS_MSG_DIALOG_YES, "Yes"),
    MAKE_STRING(RSX_OVERLAYS_MSG_DIALOG_NO, "No"),
    MAKE_STRING(RSX_OVERLAYS_MSG_DIALOG_CANCEL, "Back"),
    MAKE_STRING(RSX_OVERLAYS_MSG_DIALOG_OK, "OK"),
    MAKE_STRING(RSX_OVERLAYS_SAVE_DIALOG_TITLE, "Save Dialog"),
    MAKE_STRING(RSX_OVERLAYS_SAVE_DIALOG_DELETE, "Delete Save"),
    MAKE_STRING(RSX_OVERLAYS_SAVE_DIALOG_LOAD, "Load Save"),
    MAKE_STRING(RSX_OVERLAYS_SAVE_DIALOG_SAVE, "Save"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_ACCEPT, "Enter"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_CANCEL, "Back"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_SPACE, "Space"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_BACKSPACE, "Backspace"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_SHIFT, "Shift"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_ENTER_TEXT, "[Enter Text]"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_ENTER_PASSWORD, "[Enter Password]"),
    MAKE_STRING(RSX_OVERLAYS_MEDIA_DIALOG_TITLE, "Select media"),
    MAKE_STRING(RSX_OVERLAYS_MEDIA_DIALOG_TITLE_PHOTO_IMPORT,
                "Select photo to import"),
    MAKE_STRING(RSX_OVERLAYS_MEDIA_DIALOG_EMPTY, "No media found."),
    MAKE_STRING(RSX_OVERLAYS_LIST_SELECT, "Enter"),
    MAKE_STRING(RSX_OVERLAYS_LIST_CANCEL, "Back"),
    MAKE_STRING(RSX_OVERLAYS_LIST_DENY, "Deny"),
    MAKE_STRING(CELL_OSK_DIALOG_TITLE, "On Screen Keyboard"),
    MAKE_STRING(
        CELL_OSK_DIALOG_BUSY,
        "The Home Menu can't be opened while the On Screen Keyboard is busy!"),
    MAKE_STRING(CELL_SAVEDATA_CB_BROKEN, "Error - Save data corrupted"),
    MAKE_STRING(CELL_SAVEDATA_CB_FAILURE, "Error - Failed to save or load"),
    MAKE_STRING(CELL_SAVEDATA_CB_NO_DATA, "Error - Save data cannot be found"),
    MAKE_STRING(CELL_SAVEDATA_NO_DATA, "There is no saved data."),
    MAKE_STRING(CELL_SAVEDATA_NEW_SAVED_DATA_TITLE, "New Saved Data"),
    MAKE_STRING(CELL_SAVEDATA_NEW_SAVED_DATA_SUB_TITLE,
                "Select to create a new entry"),
    MAKE_STRING(CELL_SAVEDATA_SAVE_CONFIRMATION,
                "Do you want to save this data?"),
    MAKE_STRING(CELL_SAVEDATA_AUTOSAVE, "Saving..."),
    MAKE_STRING(CELL_SAVEDATA_AUTOLOAD, "Loading..."),
    MAKE_STRING(
        CELL_CROSS_CONTROLLER_FW_MSG,
        "If your system software version on the PS Vita system is earlier than "
        "1.80, you must update the system software to the latest version."),
    MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_TITLE, "Select Message"),
    MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_TITLE_INVITE, "Select Invite"),
    MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_TITLE_ADD_FRIEND, "Add Friend"),
    MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_FROM, "From:"),
    MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_SUBJECT, "Subject:"),
    MAKE_STRING(CELL_NP_SENDMESSAGE_DIALOG_TITLE, "Select Message To Send"),
    MAKE_STRING(CELL_NP_SENDMESSAGE_DIALOG_TITLE_INVITE, "Send Invite"),
    MAKE_STRING(CELL_NP_SENDMESSAGE_DIALOG_TITLE_ADD_FRIEND, "Add Friend"),
    MAKE_STRING(RECORDING_ABORTED, "Recording aborted!"),
    MAKE_STRING(RPCN_NO_ERROR, "RPCN: No Error"),
    MAKE_STRING(RPCN_ERROR_INVALID_INPUT,
                "RPCN: Invalid Input (Wrong Host/Port)"),
    MAKE_STRING(RPCN_ERROR_WOLFSSL, "RPCN Connection Error: WolfSSL Error"),
    MAKE_STRING(RPCN_ERROR_RESOLVE, "RPCN Connection Error: Resolve Error"),
    MAKE_STRING(RPCN_ERROR_CONNECT, "RPCN Connection Error"),
    MAKE_STRING(RPCN_ERROR_LOGIN_ERROR,
                "RPCN Login Error: Identification Error"),
    MAKE_STRING(RPCN_ERROR_ALREADY_LOGGED,
                "RPCN Login Error: User Already Logged In"),
    MAKE_STRING(RPCN_ERROR_INVALID_LOGIN, "RPCN Login Error: Invalid Username"),
    MAKE_STRING(RPCN_ERROR_INVALID_PASSWORD,
                "RPCN Login Error: Invalid Password"),
    MAKE_STRING(RPCN_ERROR_INVALID_TOKEN, "RPCN Login Error: Invalid Token"),
    MAKE_STRING(RPCN_ERROR_INVALID_PROTOCOL_VERSION,
                "RPCN Misc Error: Protocol Version Error (outdated RPCS3?)"),
    MAKE_STRING(RPCN_ERROR_UNKNOWN, "RPCN: Unknown Error"),
    MAKE_STRING(RPCN_SUCCESS_LOGGED_ON, "Successfully logged on RPCN!"),
    MAKE_STRING(HOME_MENU_TITLE, "Home Menu"),
    MAKE_STRING(HOME_MENU_EXIT_GAME, "Exit Game"),
    MAKE_STRING(HOME_MENU_RESUME, "Resume Game"),
    MAKE_STRING(HOME_MENU_FRIENDS, "Friends"),
    MAKE_STRING(HOME_MENU_FRIENDS_REQUESTS, "Pending Friend Requests"),
    MAKE_STRING(HOME_MENU_FRIENDS_BLOCKED, "Blocked Users"),
    MAKE_STRING(HOME_MENU_FRIENDS_STATUS_ONLINE, "Online"),
    MAKE_STRING(HOME_MENU_FRIENDS_STATUS_OFFLINE, "Offline"),
    MAKE_STRING(HOME_MENU_FRIENDS_STATUS_BLOCKED, "Blocked"),
    MAKE_STRING(HOME_MENU_FRIENDS_REQUEST_SENT, "You sent a friend request"),
    MAKE_STRING(HOME_MENU_FRIENDS_REQUEST_RECEIVED,
                "Sent you a friend request"),
    MAKE_STRING(HOME_MENU_FRIENDS_REJECT_REQUEST, "Reject Request"),
    MAKE_STRING(HOME_MENU_FRIENDS_NEXT_LIST, "Next list"),
    MAKE_STRING(HOME_MENU_RESTART, "Restart Game"),
    MAKE_STRING(HOME_MENU_SETTINGS, "Settings"),
    MAKE_STRING(HOME_MENU_SETTINGS_SAVE, "Save custom configuration?"),
    MAKE_STRING(HOME_MENU_SETTINGS_SAVE_BUTTON, "Save"),
    MAKE_STRING(HOME_MENU_SETTINGS_DISCARD,
                "Discard the current settings' changes?"),
    MAKE_STRING(HOME_MENU_SETTINGS_DISCARD_BUTTON, "Discard"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO, "Audio"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_MASTER_VOLUME, "Master Volume"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_BACKEND, "Audio Backend"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_BUFFERING, "Enable Buffering"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_BUFFER_DURATION,
                "Desired Audio Buffer Duration"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_TIME_STRETCHING,
                "Enable Time Stretching"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_TIME_STRETCHING_THRESHOLD,
                "Time Stretching Threshold"),
    MAKE_STRING(HOME_MENU_SETTINGS_VIDEO, "Video"),
    MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_FRAME_LIMIT, "Frame Limit"),
    MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_ANISOTROPIC_OVERRIDE,
                "Anisotropic Filter Override"),
    MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_OUTPUT_SCALING, "Output Scaling"),
    MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_RCAS_SHARPENING,
                "FidelityFX CAS Sharpening Intensity"),
    MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_STRETCH_TO_DISPLAY,
                "Stretch To Display Area"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT, "Input"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_BACKGROUND_INPUT,
                "Background Input Enabled"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_KEEP_PADS_CONNECTED,
                "Keep Pads Connected"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_SHOW_PS_MOVE_CURSOR,
                "Show PS Move Cursor"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_CAMERA_FLIP, "Camera Flip"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_PAD_MODE, "Pad Handler Mode"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_PAD_SLEEP, "Pad Handler Sleep"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_FAKE_MOVE_ROTATION_CONE_H,
                "Fake PS Move Rotation Cone (Horizontal)"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_FAKE_MOVE_ROTATION_CONE_V,
                "Fake PS Move Rotation Cone (Vertical)"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED, "Advanced"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_PREFERRED_SPU_THREADS,
                "Preferred SPU Threads"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_MAX_CPU_PREEMPTIONS,
                "Max Power Saving CPU-Preemptions"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_ACCURATE_RSX_RESERVATION_ACCESS,
                "Accurate RSX reservation access"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_SLEEP_TIMERS_ACCURACY,
                "Sleep Timers Accuracy"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_MAX_SPURS_THREADS,
                "Max SPURS Threads"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_DRIVER_WAKE_UP_DELAY,
                "Driver Wake-Up Delay"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_VBLANK_FREQUENCY,
                "VBlank Frequency"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_VBLANK_NTSC, "VBlank NTSC Fixup"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS, "Overlays"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_TROPHY_POPUPS,
                "Show Trophy Popups"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_RPCN_POPUPS,
                "Show RPCN Popups"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_SHADER_COMPILATION_HINT,
                "Show Shader Compilation Hint"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_PPU_COMPILATION_HINT,
                "Show PPU Compilation Hint"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_AUTO_SAVE_LOAD_HINT,
                "Show Autosave/Autoload Hint"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_PRESSURE_INTENSITY_TOGGLE_HINT,
                "Show Pressure Intensity Toggle Hint"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_ANALOG_LIMITER_TOGGLE_HINT,
                "Show Analog Limiter Toggle Hint"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_MOUSE_AND_KB_TOGGLE_HINT,
                "Show Mouse And Keyboard Toggle Hint"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY, "Performance Overlay"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_ENABLE,
                "Enable Performance Overlay"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_ENABLE_FRAMERATE_GRAPH,
                "Enable Framerate Graph"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_ENABLE_FRAMETIME_GRAPH,
                "Enable Frametime Graph"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_DETAIL_LEVEL,
                "Detail level"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FRAMERATE_DETAIL_LEVEL,
                "Framerate Graph Detail Level"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FRAMETIME_DETAIL_LEVEL,
                "Frametime Graph Detail Level"),
    MAKE_STRING(
        HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FRAMERATE_DATAPOINT_COUNT,
        "Framerate Datapoints"),
    MAKE_STRING(
        HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FRAMETIME_DATAPOINT_COUNT,
        "Frametime Datapoints"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_UPDATE_INTERVAL,
                "Metrics Update Interval"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_POSITION, "Position"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_CENTER_X,
                "Center Horizontally"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_CENTER_Y,
                "Center Vertically"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_MARGIN_X,
                "Horizontal Margin"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_MARGIN_Y,
                "Vertical Margin"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FONT_SIZE, "Font Size"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_OPACITY, "Opacity"),
    MAKE_STRING(HOME_MENU_SETTINGS_DEBUG, "Debug"),
    MAKE_STRING(HOME_MENU_SETTINGS_DEBUG_OVERLAY, "Debug Overlay"),
    MAKE_STRING(HOME_MENU_SETTINGS_DEBUG_INPUT_OVERLAY, "Input Debug Overlay"),
    MAKE_STRING(HOME_MENU_SETTINGS_DEBUG_DISABLE_VIDEO_OUTPUT,
                "Disable Video Output"),
    MAKE_STRING(HOME_MENU_SETTINGS_DEBUG_TEXTURE_LOD_BIAS,
                "Texture LOD Bias Addend"),
    MAKE_STRING(HOME_MENU_SCREENSHOT, "Take Screenshot"),
    MAKE_STRING(HOME_MENU_SAVESTATE, "SaveState"),
    MAKE_STRING(HOME_MENU_SAVESTATE_SAVE, "Save Emulation State"),
    MAKE_STRING(HOME_MENU_SAVESTATE_AND_EXIT, "Save Emulation State And Exit"),
    MAKE_STRING(HOME_MENU_RELOAD_SAVESTATE, "Reload Last Emulation State"),
    MAKE_STRING(HOME_MENU_RECORDING, "Start/Stop Recording"),
    MAKE_STRING(HOME_MENU_TROPHIES, "Trophies"),
    MAKE_STRING(HOME_MENU_TROPHY_HIDDEN_TITLE, "Hidden trophy"),
    MAKE_STRING(HOME_MENU_TROPHY_HIDDEN_DESCRIPTION, "This trophy is hidden"),
    MAKE_STRING(HOME_MENU_TROPHY_PLATINUM_RELEVANT, "Platinum relevant"),
    MAKE_STRING(HOME_MENU_TROPHY_GRADE_BRONZE, "Bronze"),
    MAKE_STRING(HOME_MENU_TROPHY_GRADE_SILVER, "Silver"),
    MAKE_STRING(HOME_MENU_TROPHY_GRADE_GOLD, "Gold"),
    MAKE_STRING(HOME_MENU_TROPHY_GRADE_PLATINUM, "Platinum"),
    MAKE_STRING(AUDIO_MUTED, "Audio muted"),
    MAKE_STRING(AUDIO_UNMUTED, "Audio unmuted"),
    MAKE_STRING(PROGRESS_DIALOG_PROGRESS, "Progress:"),
    MAKE_STRING(PROGRESS_DIALOG_PROGRESS_ANALYZING, "Progress: analyzing..."),
    MAKE_STRING(PROGRESS_DIALOG_REMAINING, "remaining"),
    MAKE_STRING(PROGRESS_DIALOG_DONE, "done"),
    MAKE_STRING(PROGRESS_DIALOG_FILE, "file"),
    MAKE_STRING(PROGRESS_DIALOG_MODULE, "module"),
    MAKE_STRING(PROGRESS_DIALOG_OF, "of"),
    MAKE_STRING(PROGRESS_DIALOG_PLEASE_WAIT, "Please wait"),
    MAKE_STRING(PROGRESS_DIALOG_STOPPING_PLEASE_WAIT,
                "Stopping. Please wait..."),
    MAKE_STRING(PROGRESS_DIALOG_SAVESTATE_PLEASE_WAIT,
                "Creating savestate. Please wait..."),
    MAKE_STRING(PROGRESS_DIALOG_SCANNING_PPU_EXECUTABLE,
                "Scanning PPU Executable..."),
    MAKE_STRING(PROGRESS_DIALOG_ANALYZING_PPU_EXECUTABLE,
                "Analyzing PPU Executable..."),
    MAKE_STRING(PROGRESS_DIALOG_SCANNING_PPU_MODULES,
                "Scanning PPU Modules..."),
    MAKE_STRING(PROGRESS_DIALOG_LOADING_PPU_MODULES, "Loading PPU Modules..."),
    MAKE_STRING(PROGRESS_DIALOG_COMPILING_PPU_MODULES,
                "Compiling PPU Modules..."),
    MAKE_STRING(PROGRESS_DIALOG_LINKING_PPU_MODULES, "Linking PPU Modules..."),
    MAKE_STRING(PROGRESS_DIALOG_APPLYING_PPU_CODE, "Applying PPU Code..."),
    MAKE_STRING(PROGRESS_DIALOG_BUILDING_SPU_CACHE, "Building SPU Cache..."),
    MAKE_STRING(EMULATION_PAUSED_RESUME_WITH_START,
                "Press and hold the START button to resume"),
    MAKE_STRING(EMULATION_RESUMING, "Resuming...!"),
    MAKE_STRING(EMULATION_FROZEN,
                "The PS3 application has likely crashed, you can close it."),
    MAKE_STRING(
        SAVESTATE_FAILED_DUE_TO_SAVEDATA,
        "SaveState failed: Game saving is in progress, wait until finished."),
    MAKE_STRING(SAVESTATE_FAILED_DUE_TO_VDEC,
                "SaveState failed: VDEC-base video/cutscenes are in order, "
                "wait for them to end or enable libvdec.sprx."),
    MAKE_STRING(SAVESTATE_FAILED_DUE_TO_MISSING_SPU_SETTING,
                "SaveState failed: Failed to lock SPU state, enabling "
                "SPU-Compatible mode may fix it."),
    MAKE_STRING(SAVESTATE_FAILED_DUE_TO_SPU,
                "SaveState failed: Failed to lock SPU state, using SPU ASMJIT "
                "will fix it."),
    MAKE_STRING(INVALID, "Invalid"),
};

enum GameFlags {
  kGameFlagLocked = 1 << 0,
  kGameFlagTrial = 1 << 1,
};

struct GameInfo {
  std::string path;
  std::string name;
  std::string iconPath;
  int flags = 0;
  std::string sourceUri;
  std::string version;
  // PARAM.SFO TITLE_ID (e.g. "BLES01253"). The same title can be reported
  // from more than one source (an installed copy, a raw ISO, a loose
  // folder) - the Kotlin side uses this, not path, as the game's real
  // identity when deciding whether two reports are "the same game".
  std::string titleId;
  // PARAM.SFO CATEGORY ("DG" disc game, "HG" HDD game, "GD" disc-game
  // update, ...). Lets the app rank which source should represent a title
  // when more than one is registered: a full installed game beats a raw
  // ISO, which beats a bare "GD" update (not standalone-bootable on its
  // own - an ISO boot applies it automatically from dev_hdd0/game).
  std::string category;
};

class Progress {
  JNIEnv *env;
  jlong progressId;
  jclass progressRepositoryClass;
  jmethodID onProgressEventMethodId;

public:
  Progress(JNIEnv *env, jlong progressId) : env(env), progressId(progressId) {
    progressRepositoryClass =
        ensure(env->FindClass("net/rpcsx/ProgressRepository"));
    onProgressEventMethodId = env->GetStaticMethodID(
        progressRepositoryClass, "onProgressEvent", "(JJJLjava/lang/String;)Z");
  }

  bool report(jlong value, jlong max, const std::string &message = {}) {
    return env->CallStaticBooleanMethod(
        progressRepositoryClass, onProgressEventMethodId, progressId, value,
        max, message.empty() ? nullptr : wrap(env, message));
  }

  void failure(const std::string &message = {}) { report(-1, 0, message); }

  void success(jlong value, const std::string &message = {}) {
    value = std::max<jlong>(value, 1);
    report(value, value, message);
  }

  jlong getProgressId() const { return progressId; }
};

static void sendFirmwareInstalled(JNIEnv *env, const std::string &version) {
  auto fwRepositoryClass =
      ensure(env->FindClass("net/rpcsx/FirmwareRepository"));
  auto methodId = ensure(env->GetStaticMethodID(
      fwRepositoryClass, "onFirmwareInstalled", "(Ljava/lang/String;)V"));

  env->CallStaticVoidMethod(fwRepositoryClass, methodId, wrap(env, version));
}

static void sendFirmwareCompiled(JNIEnv *env, const std::string &version) {
  auto fwRepositoryClass =
      ensure(env->FindClass("net/rpcsx/FirmwareRepository"));
  auto methodId = ensure(env->GetStaticMethodID(
      fwRepositoryClass, "onFirmwareCompiled", "(Ljava/lang/String;)V"));

  env->CallStaticVoidMethod(fwRepositoryClass, methodId, wrap(env, version));
}

static void sendGameInfo(JNIEnv *env, jlong progressId,
                         std::span<const GameInfo> infos) {
  auto gameRepositoryClass = ensure(env->FindClass("net/rpcsx/GameRepository"));
  auto addMethodId = ensure(env->GetStaticMethodID(
      gameRepositoryClass, "add", "([Lnet/rpcsx/GameInfo;J)V"));
  auto gameClass = ensure(env->FindClass("net/rpcsx/GameInfo"));

  // Prefer the newest constructor (adds category), falling back through
  // older arities detected at runtime via a non-throwing GetMethodID - so
  // this one core binary keeps working against an app build that hasn't
  // been rebuilt against the newest GameInfo yet (each miss above the one
  // that matches raises a NoSuchMethodError that must be cleared before the
  // next JNI call, or it leaks into and aborts an unrelated one).
  jmethodID gameConstructorV4 = env->GetMethodID(
      gameClass, "<init>",
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ILjava/lang/"
      "String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
  if (gameConstructorV4 == nullptr) {
    env->ExceptionClear();
  }

  jmethodID gameConstructorV3 =
      gameConstructorV4 ? nullptr
                        : env->GetMethodID(
                              gameClass, "<init>",
                              "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ILjava/lang/"
                              "String;Ljava/lang/String;Ljava/lang/String;)V");
  if (gameConstructorV3 == nullptr && gameConstructorV4 == nullptr) {
    env->ExceptionClear();
  }

  jmethodID gameConstructorV2 =
      (gameConstructorV4 || gameConstructorV3)
          ? nullptr
          : env->GetMethodID(
                gameClass, "<init>",
                "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ILjava/lang/"
                "String;Ljava/lang/String;)V");
  if (gameConstructorV2 == nullptr && gameConstructorV3 == nullptr &&
      gameConstructorV4 == nullptr) {
    env->ExceptionClear();
  }

  jmethodID gameConstructor =
      (gameConstructorV4 || gameConstructorV3 || gameConstructorV2)
          ? nullptr
          : ensure(env->GetMethodID(
                gameClass, "<init>",
                "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ILjava/lang/"
                "String;)V"));

  std::vector<jobject> objects;
  objects.reserve(infos.size());

  for (const auto &info : infos) {
    auto path = Emu.GetCallbacks().resolve_path(info.path);
    if (path.ends_with('/')) {
      path.resize(path.size() - 1);
    }

    auto iconPath = Emu.GetCallbacks().resolve_path(info.iconPath);
    auto sourceUri = info.sourceUri.empty() ? nullptr : wrap(env, info.sourceUri);
    auto version = info.version.empty() ? nullptr : wrap(env, info.version);
    auto titleId = info.titleId.empty() ? nullptr : wrap(env, info.titleId);

    if (gameConstructorV4) {
      objects.push_back(env->NewObject(
          gameClass, gameConstructorV4, wrap(env, path), wrap(env, info.name),
          wrap(env, iconPath), jint(info.flags), sourceUri, version, titleId,
          info.category.empty() ? nullptr : wrap(env, info.category)));
    } else if (gameConstructorV3) {
      objects.push_back(env->NewObject(
          gameClass, gameConstructorV3, wrap(env, path), wrap(env, info.name),
          wrap(env, iconPath), jint(info.flags), sourceUri, version, titleId));
    } else if (gameConstructorV2) {
      objects.push_back(env->NewObject(
          gameClass, gameConstructorV2, wrap(env, path), wrap(env, info.name),
          wrap(env, iconPath), jint(info.flags), sourceUri, version));
    } else {
      objects.push_back(env->NewObject(
          gameClass, gameConstructor, wrap(env, path), wrap(env, info.name),
          wrap(env, iconPath), jint(info.flags), sourceUri));
    }
  }

  auto result = env->NewObjectArray(objects.size(), gameClass, nullptr);

  for (std::size_t i = 0; i < objects.size(); ++i) {
    env->SetObjectArrayElement(result, i, objects[i]);
  }

  env->CallStaticVoidMethod(gameRepositoryClass, addMethodId, result,
                            progressId);
}

static void sendVshBootable(JNIEnv *env, jlong progressId) {
  auto dev_flash = g_cfg_vfs.get_dev_flash();

  sendGameInfo(
      env, progressId,
      {{GameInfo{
          .path = dev_flash + "/vsh/module/vsh.self",
          .name = "VSH",
          .iconPath = dev_flash + "vsh/resource/explore/icon/icon_home.png",
      }}});
}

static bool tryUnlockGame(const psf::registry &psf) {
  auto contentId = psf::get_string(psf, "CONTENT_ID");

  if (contentId.empty()) {
    return true;
  }

  const auto licenseDir = fmt::format(
      "%shome/%s/exdata/", rpcs3::utils::get_hdd0_dir(), Emu.GetUsr());

  const auto licenseFile = fmt::format("%s%s", licenseDir, contentId);
  if (std::filesystem::is_regular_file(licenseFile + ".rap")) {
    return true;
  }

  if (std::filesystem::is_regular_file(licenseFile + ".edat")) {
    return true;
  }

  return false;
}

static void collectGamePaths(std::vector<std::string> &paths,
                             const std::string &rootDir) {
  std::error_code ec;
  std::vector<std::filesystem::path> workList;
  workList.reserve(32);
  if (!std::filesystem::is_directory(rootDir)) {
    auto rootPath = std::filesystem::path(rootDir).parent_path();
    if (rootPath.filename() == "USRDIR") {
      rootPath = rootPath.parent_path();
    }
    if (rootPath.filename() == "PS3_GAME") {
      rootPath = rootPath.parent_path();
    }

    workList.push_back(rootPath);
  } else {
    workList.push_back(rootDir);
  }

  while (!workList.empty()) {
    auto dir = std::move(workList.back());
    workList.pop_back();

    for (auto &entry : std::filesystem::directory_iterator(dir, ec)) {
      if (entry.is_directory()) {
        if (entry.path().filename() != "C00") {
          workList.push_back(entry.path());
        }

        continue;
      }

      if (entry.is_regular_file() && entry.path().filename() == "PARAM.SFO") {
        paths.push_back(entry.path().parent_path().string());
        continue;
      }
    }
  }
}

static std::string locateEbootPath(std::string_view root) {
  if (std::filesystem::is_regular_file(root)) {
    return std::string(root);
  }

  for (auto suffix : {
           "/EBOOT.BIN",
           "/USRDIR/EBOOT.BIN",
           "/USRDIR/ISO.BIN.EDAT",
           "/PS3_GAME/USRDIR/EBOOT.BIN",
       }) {
    auto tryPath = std::string(root);
    tryPath += suffix;

    if (std::filesystem::is_regular_file(tryPath)) {
      return tryPath;
    }
  }

  return {};
}

static std::string locateParamSfoPath(std::string_view root) {
  if (std::filesystem::is_regular_file(root)) {
    return std::string(root);
  }

  for (auto suffix : {
           "/PARAM.SFO",
           "/PS3_GAME/PARAM.SFO",
       }) {
    auto tryPath = std::string(root);
    tryPath += suffix;

    if (std::filesystem::is_regular_file(tryPath)) {
      return tryPath;
    }
  }

  return {};
}

// If the game dir holds a .iso (installed by installIso), the game boots from
// the image directly: Emu.BootGame() on an iso path mounts it on the fly.
static std::string findIsoInDir(const std::string &dir) {
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }

    auto ext = entry.path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (ext == ".iso") {
      return entry.path().string();
    }
  }

  return {};
}

static std::optional<GameInfo>
fetchGameInfo(const psf::registry &psf,
              std::filesystem::path psfRootPath = {}) {
  auto titleId = std::string(psf::get_string(psf, "TITLE_ID"));
  auto name = std::string(psf::get_string(psf, "TITLE"));
  auto bootable = psf::get_integer(psf, "BOOTABLE", 0);
  auto category = psf::get_string(psf, "CATEGORY");
  auto version = std::string(psf::get_string(psf, "APP_VER"));
  if (version.empty()) {
    version = std::string(psf::get_string(psf, "VERSION"));
  }

  // An update/patch PKG's own PARAM.SFO is commonly BOOTABLE=0 (it only
  // patches files into the already-installed, already-bootable game
  // directory) - not a playable entry in its own right, so it's simply
  // skipped here instead of being reported as (or merged into) a game.
  // Whatever it changed is picked up next time the real entry - installed
  // copy, ISO or folder - is scanned, via the version lookup below.
  if (!bootable || titleId.empty()) {
    return {};
  }

  bool isDiscGame = category == "DG";

  std::string path;

  if (!isDiscGame) {
    path = rpcs3::utils::get_hdd0_dir() + "game/" + titleId + "/";
  } else {
    if (psfRootPath.empty()) {
      path = fs::get_config_dir() + "games/" + titleId + "/";
    } else {
      // Locate game root path
      if (psfRootPath.filename() == "USRDIR") {
        psfRootPath = psfRootPath.parent_path();
      }

      if (psfRootPath.filename() == "PS3_GAME") {
        psfRootPath = psfRootPath.parent_path();
      }

      path = psfRootPath;
      if (!path.ends_with('/')) {
        path += '/';
      }
    }
  }

  auto dataPath = isDiscGame ? path + "PS3_GAME/" : path;
  auto iconPath = dataPath + "ICON0.PNG";
  auto moviePath = dataPath + "ICON1.PAM";

  int flags = 0;

  if (!isDiscGame) {
    auto ebootPath = locateEbootPath(path);

    bool isLocked = false;

    if (!ebootPath.empty()) {
      if (fs::file eboot{ebootPath};
          eboot && eboot.size() >= 4 && eboot.read<u32>() == "SCE\0"_u32) {
        isLocked = !decrypt_self(eboot);
      }
    }

    if (isLocked) {
      flags |= kGameFlagLocked;
      rpcsx_android.warning("game %s is locked", path);
    }

    auto c00Path = path + "/C00";

    bool isTrial = std::filesystem::is_directory(c00Path);

    if (isTrial) {
      if (!tryUnlockGame(psf)) {
        flags |= kGameFlagTrial;
        rpcsx_android.warning("game %s is trial", path);
      } else {
        auto c00IconPath = c00Path + "/ICON0.PNG";
        if (std::filesystem::is_regular_file(c00IconPath)) {
          iconPath = c00IconPath;
        }

        auto c00SfoPath = c00Path + "/PARAM.SFO";

        if (std::filesystem::is_regular_file(c00IconPath)) {
          auto c00Sfo = psf::load_object(c00SfoPath);
          titleId = psf::get_string(c00Sfo, "TITLE_ID", titleId);
          name = psf::get_string(c00Sfo, "TITLE", name);
        }
      }
    }
  }

  if (isDiscGame) {
    // On-the-fly disc image: boot the .iso itself if present
    if (auto isoPath = findIsoInDir(path); !isoPath.empty()) {
      path = std::move(isoPath);
    }
  }

  // Prefer an installed update's version (dev_hdd0/game/<id>/PARAM.SFO) over
  // this entry's own disc/base version - that's the effective version the
  // game actually runs at once a patch is installed, regardless of whether
  // this entry itself is the installed copy, a raw ISO or a loose folder
  // (RPCS3 applies dev_hdd0/game/<id> data to any of those at boot by
  // TITLE_ID). For an installed HDD game this is the same file - a no-op.
  if (!titleId.empty()) {
    const auto updateSfoPath =
        rpcs3::utils::get_hdd0_dir() + "game/" + titleId + "/PARAM.SFO";
    if (fs::is_file(updateSfoPath)) {
      auto updateSfo = psf::load_object(updateSfoPath);
      auto updateVersion = std::string(psf::get_string(updateSfo, "APP_VER"));
      if (!updateVersion.empty()) {
        version = std::move(updateVersion);
      }
    }
  }

  return GameInfo{
      .path = std::move(path),
      .name = std::move(name),
      .iconPath = std::move(iconPath),
      .flags = flags,
      .version = std::move(version),
      .titleId = std::move(titleId),
      .category = std::string(category),
  };
}

static void collectGameInfo(JNIEnv *env, jlong progressId,
                            const std::vector<std::string> &rootDirs) {
  std::vector<std::string> paths;
  for (auto &&rootDir : rootDirs) {
    collectGamePaths(paths, rootDir);

    rpcsx_android.notice("collectGameInfo: processed %s", rootDir);
  }

  rpcsx_android.notice("collectGameInfo: found %d paths", paths.size());

  Progress progress(env, progressId);
  progress.report(0, paths.size());

  std::vector<GameInfo> gameInfos;
  gameInfos.reserve(10);
  std::size_t processed = 0;

  auto submit = [&] {
    if (gameInfos.empty()) {
      return;
    }

    sendGameInfo(env, progressId, gameInfos);
    progress.report(processed, paths.size());
    gameInfos.clear();
  };

  for (auto &&path : paths) {
    processed++;

    if (!std::filesystem::is_regular_file(path + "/PARAM.SFO")) {
      continue;
    }

    const auto psf = psf::load_object(path + "/PARAM.SFO");

    rpcsx_android.notice("collectGameInfo: sfo at %s", path);

    if (auto gameInfo = fetchGameInfo(psf, path)) {
      gameInfos.push_back(std::move(*gameInfo));

      if (gameInfos.size() >= 10) {
        submit();
      }
    }
  }

  submit();

  progress.success(processed);
}

class MainThreadProcessor {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::pair<std::function<void(JNIEnv *)>, atomic_t<u32> *>> queue;

public:
  void push(std::function<void(JNIEnv *)> cb, atomic_t<u32> *wakeUp = nullptr) {
    std::lock_guard lock(mutex);
    queue.push_back({std::move(cb), wakeUp});
    cv.notify_one();
  }

  void push(std::function<void()> cb, atomic_t<u32> *wakeUp = nullptr) {
    push([cb = std::move(cb)](JNIEnv *) { cb(); }, wakeUp);
  }

  void process(JNIEnv *env) {
    while (true) {
      std::function<void(JNIEnv *)> cb;
      atomic_t<u32> *wakeUp = nullptr;

      {
        std::unique_lock lock(mutex);
        if (queue.empty()) {
          cv.wait(lock);
          continue;
        }

        auto item = std::move(queue.front());
        queue.pop_front();

        cb = std::move(item.first);
        wakeUp = item.second;
      }

      cb(env);
      if (wakeUp) {
        *wakeUp = true;
        wakeUp->notify_all();
      }
    }
  }
} static g_mainThreadProcessor;

static void invokeAsync(std::function<void(JNIEnv *)> cb) {
  g_mainThreadProcessor.push(std::move(cb));
}

static void invokeSync(std::function<void(JNIEnv *)> cb) {
  atomic_t<u32> wakeup{false};
  g_mainThreadProcessor.push(std::move(cb), &wakeup);

  while (wakeup.load() == false) {
    wakeup.wait(false);
  }
}

struct ProgressMessageDialog : MsgDialogBase {
  jlong progressId;
  jlong value = 0;
  jlong max = 0;

  ProgressMessageDialog(jlong progressId) : progressId(progressId) {}

  void Create(const std::string &msg, const std::string &title) override {
    rpcsx_android.warning("ProgressMessageDialog::Create(%s, %s)", msg, title);
    max = 100;
    invokeSync([this, &msg](JNIEnv *env) {
      Progress progress(env, progressId);
      progress.report(0, 0, msg);
    });
  }

  jlong getValue() const {
    return value == max && max != 0 ? value - 1 : value;
  }

  void Close(bool success) override {
    rpcsx_android.warning("ProgressMessageDialog::Close(%s)", success);
    invokeSync([this](JNIEnv *env) {
      Progress progress(env, progressId);
      progress.report(0, 0);
    });

    //   Progress progress(env, progressId);
    //   if (success) {
    //     progress.success(0);
    //   } else {
    //     progress.failure();
    //   }
    // });
  }

  void SetMsg(const std::string &msg) override {
    rpcsx_android.warning("ProgressMessageDialog::SetMsg(%s)", msg);
    invokeSync([this, msg](JNIEnv *env) {
      Progress(env, progressId).report(getValue(), max, msg);
    });
  }

  void ProgressBarSetMsg(u32 progressBarIndex,
                         const std::string &msg) override {
    rpcsx_android.warning("ProgressMessageDialog::ProgressBarSetMsg(%d, %s)",
                          progressBarIndex, msg);
    if (progressBarIndex != 0) {
      report_fatal_error("Unexpected progress index in progress dialog");
    }

    invokeSync([this, msg](JNIEnv *env) {
      Progress(env, progressId).report(getValue(), max, msg);
    });
  }

  void ProgressBarReset(u32 progressBarIndex) override {
    rpcsx_android.warning("ProgressMessageDialog::ProgressBarReset(%d)",
                          progressBarIndex);

    if (progressBarIndex != 0) {
      report_fatal_error("Unexpected progress index in progress dialog");
    }

    value = 0;
    invokeSync(
        [this](JNIEnv *env) { Progress(env, progressId).report(value, max); });
  }

  void ProgressBarInc(u32 progressBarIndex, u32 delta) override {
    rpcsx_android.warning("ProgressMessageDialog::ProgressBarInc(%d, %d)",
                          progressBarIndex, delta);

    if (progressBarIndex != 0) {
      report_fatal_error("Unexpected progress index in progress dialog");
    }

    value += delta;

    invokeSync([this](JNIEnv *env) {
      Progress(env, progressId).report(getValue(), max);
    });
  }

  void ProgressBarSetValue(u32 progressBarIndex, u32 value) override {
    rpcsx_android.warning("ProgressMessageDialog::ProgressBarSetValue(%d, %d)",
                          progressBarIndex, value);

    if (progressBarIndex != 0) {
      report_fatal_error("Unexpected progress index in progress dialog");
    }

    this->value = value;

    invokeSync([this](JNIEnv *env) {
      Progress(env, progressId).report(getValue(), max);
    });
  }
  void ProgressBarSetLimit(u32 index, u32 limit) override {
    rpcsx_android.warning("ProgressMessageDialog::ProgressBarSetLimit(%d, %d)",
                          index, limit);

    if (index != 0) {
      report_fatal_error("Unexpected progress index in progress dialog");
    }

    max = limit;

    invokeSync([this](JNIEnv *env) {
      Progress(env, progressId).report(getValue(), max);
    });
  }
};

struct UiMessageDialog : MsgDialogBase {
  // FIXME: implement

  void Create(const std::string &msg, const std::string &title) override {}
  void Close(bool success) override {}
  void SetMsg(const std::string &msg) override {}
  void ProgressBarSetMsg(u32 progressBarIndex,
                         const std::string &msg) override {}
  void ProgressBarReset(u32 progressBarIndex) override {}
  void ProgressBarInc(u32 progressBarIndex, u32 delta) override {}
  void ProgressBarSetValue(u32 progressBarIndex, u32 value) override {}
  void ProgressBarSetLimit(u32 index, u32 limit) override {}
};

struct MessageDialog : MsgDialogBase {
  std::unique_ptr<MsgDialogBase> impl = nullptr;

  void Create(const std::string &msg, const std::string &title) override {
    auto progressId = s_pendingProgressId.load();

    rpcsx_android.warning("MessageDialog::Create(%s, %s): source %s, id %d",
                          msg, title, source, progressId);

    if (progressId != -1) {
      impl = std::make_unique<ProgressMessageDialog>(progressId);
    } else {
      impl = std::make_unique<UiMessageDialog>();
    }

    impl->type = type;
    impl->source = source;
    impl->Create(msg, title);
  }

  void Close(bool success) override { impl->Close(success); }

  void SetMsg(const std::string &msg) override { impl->SetMsg(msg); }

  void ProgressBarSetMsg(u32 progressBarIndex,
                         const std::string &msg) override {
    impl->ProgressBarSetMsg(progressBarIndex, msg);
  }

  void ProgressBarReset(u32 progressBarIndex) override {
    impl->ProgressBarReset(progressBarIndex);
  }

  void ProgressBarInc(u32 progressBarIndex, u32 delta) override {
    impl->ProgressBarInc(progressBarIndex, delta);
  }

  void ProgressBarSetValue(u32 progressBarIndex, u32 value) override {
    impl->ProgressBarSetValue(progressBarIndex, value);
  }

  void ProgressBarSetLimit(u32 index, u32 limit) override {
    impl->ProgressBarSetLimit(index, limit);
  }

  static void pushPendingProgressId(jlong id) {
    jlong value = -1;

    while (!s_pendingProgressId.compare_exchange_weak(value, id)) {
      s_pendingProgressId.wait(value);
      value = -1;
    }
  }

  static bool popPendingProgressId(jlong id) {
    return s_pendingProgressId.compare_exchange_strong(id, -1);
  }

private:
  static std::atomic<jlong> s_pendingProgressId;
};

struct OverlaySaveDialog : SaveDialogBase {
  s32 ShowSaveDataList(const std::string &base_dir,
                       std::vector<SaveDataEntry> &save_entries, s32 focused,
                       u32 op, vm::ptr<CellSaveDataListSet> listSet,
                       bool enable_overlay) override {
    rpcsx_android.notice("ShowSaveDataList(save_entries=%d, focused=%d, "
                         "op=0x%x, listSet=*0x%x, enable_overlay=%d)",
                         save_entries.size(), focused, op, listSet,
                         enable_overlay);

    bool use_end = sysutil_send_system_cmd(CELL_SYSUTIL_DRAWING_BEGIN, 0) >= 0;

    auto atExit = AtExit([&] {
      if (use_end) {
        sysutil_send_system_cmd(CELL_SYSUTIL_DRAWING_END, 0);
      }
    });

    if (!use_end) {
      rpcsx_android.error(
          "ShowSaveDataList(): Not able to notify DRAWING_BEGIN callback "
          "because one has already been sent!");
    }

    if (auto manager = g_fxo->try_get<rsx::overlays::display_manager>()) {
      rpcsx_android.notice("ShowSaveDataList: Showing native UI dialog");

      s32 result = manager->create<rsx::overlays::save_dialog>()->show(
          base_dir, save_entries, focused, op, listSet, enable_overlay);

      if (result != rsx::overlays::user_interface::selection_code::error) {
        rpcsx_android.notice(
            "ShowSaveDataList: Native UI dialog returned with selection %d",
            result);

        return result;
      }

      rpcsx_android.error("ShowSaveDataList: Native UI dialog returned error");
    }

    return -2;
  }
};

class OverlayTrophyNotification : public TrophyNotificationBase {
public:
  s32 ShowTrophyNotification(
      const SceNpTrophyDetails &trophy,
      const std::vector<uchar> &trophy_icon_buffer) override {
    if (auto manager = g_fxo->try_get<rsx::overlays::display_manager>()) {
      auto popup = std::make_shared<rsx::overlays::trophy_notification>();
      return manager->add(popup, false)->show(trophy, trophy_icon_buffer);
    }

    return 0;
  }
};

std::atomic<jlong> MessageDialog::s_pendingProgressId = -1;

struct CompilationWorkload {
  jlong progressId;
  std::string path;
};

extern bool ppu_load_exec(const ppu_exec_object &, bool virtual_load,
                          const std::string &, utils::serial * = nullptr);
extern void spu_load_exec(const spu_exec_object &);
extern void spu_load_rel_exec(const spu_rel_object &);
extern void ppu_precompile(std::vector<std::string> &dir_queue,
                           std::vector<ppu_module<lv2_obj> *> *loaded_modules, bool is_fast_compilation);
extern bool ppu_initialize(const ppu_module<lv2_obj> &, bool check_only = false,
                           u64 file_size = 0);
extern void ppu_finalize(const ppu_module<lv2_obj> &);
extern bool ppu_load_rel_exec(const ppu_rel_object &);

class CompilationQueue {
  std::atomic<std::uint64_t> nextWorkTag{0};
  std::uint64_t lastProcessedTag = 0;
  std::mutex queueMutex;
  std::deque<CompilationWorkload> queue;

public:
  void push(CompilationWorkload workload) {
    {
      std::lock_guard lock(queueMutex);
      queue.push_back(std::move(workload));
    }

    nextWorkTag.fetch_add(1);
  }

  void push(Progress &progress, std::string path) {
    progress.report(0, 0);

    push({
        .progressId = progress.getProgressId(),
        .path = std::move(path),
    });
  }

  void process(JNIEnv *env) {
    while (true) {
      auto nextWorkTagValue = nextWorkTag.load();

      if (nextWorkTagValue == lastProcessedTag) {
        nextWorkTag.wait(lastProcessedTag);
      }

      if (nextWorkTagValue == lastProcessedTag || queue.empty()) {
        continue;
      }

      CompilationWorkload workload;

      {
        std::lock_guard lock(queueMutex);

        if (queue.empty()) {
          continue;
        }

        workload = std::move(queue.front());
        queue.pop_front();
      }

      impl(env, std::move(workload));
      lastProcessedTag++;
    }
  }

private:
  void impl(JNIEnv *env, CompilationWorkload workload) {
    if (workload.path.empty()) {
      Progress(env, workload.progressId).success(0);
      return;
    }

    rpcsx_android.error("Creating cache initiated, state %d",
                        (int)Emu.GetStatus(false));

    while (true) {
      auto state = Emu.GetStatus(false);

      if (state == system_state::stopped || state == system_state::ready) {
        break;
      }

      rpcsx_android.error("Creating cache wait, state %d", (int)state);
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    bool is_vsh = workload.path.ends_with("/vsh.self");

    Emu.SetTestMode();

    MessageDialog::pushPendingProgressId(workload.progressId);

    g_fxo->init<named_thread<progress_dialog_server>>();
    g_fxo->init<main_ppu_module<lv2_obj>>();
    g_fxo->init(false, nullptr);
    auto rootPath = std::filesystem::path(workload.path);

    if (is_vsh) {
      rootPath = g_cfg_vfs.get_dev_flash() + "sys/external/";
    } else {
      if (!std::filesystem::is_directory(rootPath)) {
        rootPath = rootPath.parent_path();
        if (rootPath.filename() == "USRDIR") {
          rootPath = rootPath.parent_path();
        }
      }
    }

    auto &_main = *ensure(g_fxo->try_get<main_ppu_module<lv2_obj>>());

    if (fs::is_file(workload.path)) {
      if (!is_vsh) {
        auto sfoPath = locateParamSfoPath(std::string(rootPath));

        if (!sfoPath.empty()) {
          const auto psf = psf::load_object(sfoPath);
          rpcsx_android.warning("title id is %s",
                                psf::get_string(psf, "TITLE_ID"));

          // Title ID is set during BootGame/Load, no separate setter needed
        } else {
          rpcsx_android.warning("param.sfo not found");
        }
      }

      // Compile binary first
      rpcsx_android.notice("Trying to load binary: %s", workload.path);

      fs::file src{workload.path};
      src = decrypt_self(src);

      const ppu_exec_object obj = src;

      if (obj == elf_error::ok && ppu_load_exec(obj, true, workload.path)) {
        _main.path = workload.path;
      } else {
        rpcsx_android.error("Failed to load binary '%s' (%s)", workload.path,
                            obj.get_error());
      }
    }

    std::vector<std::string> dir_queue;
    dir_queue.push_back(rootPath.string());

    for (auto &entry :
         std::filesystem::recursive_directory_iterator(rootPath)) {
      if (entry.is_directory()) {
        dir_queue.push_back(entry.path().string());
      }
    }

    std::vector<ppu_module<lv2_obj> *> mod_list;
    rpcsx_android.error("Going to analyze executable");

    // FIXME: split states
    if (!is_vsh) {
      if (_main.analyse(0, _main.elf_entry, _main.seg0_code_end,
                        _main.applied_patches, std::vector<u32>{})) {
        Emu.ConfigurePPUCache();
        Emu.SetTestMode();
        rpcsx_android.error("Going to precompile main PPU module");
        ppu_initialize(_main);
        mod_list.emplace_back(&_main);
      }
    }

    ppu_precompile(dir_queue, mod_list.empty() ? nullptr : &mod_list, false);

    rpcsx_android.error("Finalization");
    g_fxo->reset();
    Emu.Kill(false);

    MessageDialog::popPendingProgressId(workload.progressId);

    Progress(env, workload.progressId).success(0);
  }
} static g_compilationQueue;

static void setupCallbacks() {
  Emu.SetCallbacks({
      .call_from_main_thread =
          [](std::function<void()> cb, atomic_t<u32> *wake_up) {
            cb();
            if (wake_up) {
              *wake_up = true;
            }
          },
      .on_run = [](auto...) {},
      .on_pause = [](auto...) {},
      .on_resume = [](auto...) {},
      .on_stop = [](auto...) {},
      .on_ready = [](auto...) {},
      .on_missing_fw = [](auto...) {},
      .on_emulation_stop_no_response = [](auto...) {},
      .on_save_state_progress = [](auto...) {},
      .enable_disc_eject = [](auto...) {},
      .enable_disc_insert = [](auto...) {},
      .try_to_quit =
          [](bool /*force_quit*/, std::function<void()> on_exit) {
            if (on_exit) {
              on_exit();
            }
            return true;
          },
      .handle_taskbar_progress = [](auto...) {},
      .init_kb_handler =
          [](auto...) {
            ensure(g_fxo->init<KeyboardHandlerBase, NullKeyboardHandler>(
                Emu.DeserialManager()));
          },
      .init_mouse_handler =
          [](auto...) {
            ensure(g_fxo->init<MouseHandlerBase, NullMouseHandler>(
                Emu.DeserialManager()));
          },
      .init_pad_handler =
          [](auto...) {
            ensure(g_fxo->init<named_thread<pad_thread>>(nullptr, nullptr, ""));
          },
      .update_emu_settings = [](auto...) {},
      .save_emu_settings =
          [](auto...) {
            if (!Emu.GetUsedConfig().empty()) {
              // A per-game config (frontend-owned JSON, passed via the boot
              // configPath) is applied on top of the global one: dumping the
              // live g_cfg would leak its overrides into config.json.
              // Home-menu changes stay session-only in that case.
              rpcsx_android.notice(
                  "save_emu_settings: per-game config active (%s), "
                  "not persisting to the global config",
                  Emu.GetUsedConfig());
              return;
            }
            save_global_config();
          },
      .close_gs_frame = [](auto...) {},
      .get_gs_frame = [] { return std::make_unique<GraphicsFrame>(); },
      .get_camera_handler =
          [](auto...) { return std::make_shared<null_camera_handler>(); },
      .get_music_handler =
          [](auto...) { return std::make_shared<null_music_handler>(); },
      // NOTE: create_pad_handler was removed from EmuCallbacks upstream;
      // pad_thread::GetHandler() now owns this switch internally (and
      // already special-cases ANDROID for the keyboard handler), so no
      // callback is registered here anymore.
      .init_gs_render =
          [](utils::serial *ar) {
            switch (g_cfg.video.renderer.get()) {
            case video_renderer::null:
              g_fxo->init<rsx::thread, named_thread<NullGSRender>>(ar);
              break;
            case video_renderer::vulkan:
              g_fxo->init<rsx::thread, named_thread<VKGSRender>>(ar);
              break;

            default:
              break;
            }
          },
      .get_audio =
          [](auto...) {
            std::shared_ptr<AudioBackend> result;

            switch (g_cfg.audio.renderer.get()) {
            case audio_renderer::null:
              result = std::make_shared<NullAudioBackend>();
              break;

            case audio_renderer::cubeb:
            default:
              result = std::make_shared<CubebBackend>();
              break;
            }

            if (!result->Initialized()) {
              rpcsx_android.error(
                  "Audio renderer %s could not be initialized, using a Null "
                  "renderer instead. Make sure that no other application is "
                  "running that might block audio access (e.g. Netflix).",
                  result->GetName());
              result = std::make_shared<NullAudioBackend>();
            }

            rpcsx_android.notice("Active audio backend: %s", result->GetName());
            return result;
          },
      .get_audio_enumerator = [](auto...) { return nullptr; },
      .get_msg_dialog = [] { return std::make_shared<MessageDialog>(); },
      .get_osk_dialog = [](auto...) { return nullptr; },
      .get_save_dialog =
          [](auto...) { return std::make_unique<OverlaySaveDialog>(); },
      .get_sendmessage_dialog = [](auto...) { return nullptr; },
      .get_recvmessage_dialog = [](auto...) { return nullptr; },
      .get_trophy_notification_dialog =
          [](auto...) { return std::make_unique<OverlayTrophyNotification>(); },
      .get_localized_string = [](localized_string_id id,
                                 const char *) -> std::string {
        if (std::size_t(id) < std::size(g_strings)) {
          return g_strings[int(id)].first;
        }
        return "";
      },
      .get_localized_u32string = [](localized_string_id id,
                                    const char *) -> std::u32string {
        if (std::size_t(id) < std::size(g_strings)) {
          return g_strings[int(id)].second;
        }
        return U"";
      },
      .get_localized_setting = [](auto...) { return ""; },
      .get_photo_path = [](auto...) { return ""; },
      .play_sound = [](auto...) {},
      .get_image_info = [](auto...) { return false; },
      .get_scaled_image = [](auto...) { return false; },
      .resolve_path =
          [](std::string_view arg) {
            std::error_code ec;
            auto result =
                std::filesystem::weakly_canonical(
                    std::filesystem::path(fmt::replace_all(arg, "\\", "/")), ec)
                    .string();
            return ec ? std::string(arg) : result;
          },
      .get_font_dirs = [](auto...) { return std::vector<std::string>(); },
      .on_install_pkgs =
          [](const std::vector<std::string> &pkgs) {
            for (const std::string &pkg : pkgs) {
              if (!rpcs3::utils::install_pkg(pkg)) {
                rpcsx_android.error("cd install pkgs: failed to install %s",
                                    pkg);
                return false;
              }
            }
            return true;
          },
      .add_breakpoint = [](auto...) {},
      .display_sleep_control_supported = [](auto...) { return false; },
      .enable_display_sleep = [](auto...) {},
      .check_microphone_permissions = [](auto...) {},
      .make_video_source = [](auto...) { return nullptr; },
      .enable_gamemode = [](auto...) {},
      .get_database_config = [](auto...) { return ""; },
  });
}

static bool initVirtualPad(const std::shared_ptr<Pad> &pad) {
  u32 pclass_profile = 0;
  pad->Init(CELL_PAD_STATUS_CONNECTED,
            CELL_PAD_CAPABILITY_PS3_CONFORMITY |
                CELL_PAD_CAPABILITY_PRESS_MODE |
                CELL_PAD_CAPABILITY_HP_ANALOG_STICK |
                CELL_PAD_CAPABILITY_ACTUATOR //| CELL_PAD_CAPABILITY_SENSOR_MODE
            ,
            CELL_PAD_DEV_TYPE_STANDARD, CELL_PAD_PCLASS_TYPE_STANDARD,
            pclass_profile, 0, 0, 50);

  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::vector<std::set<u32>>{},
                              CELL_PAD_CTRL_UP);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::vector<std::set<u32>>{},
                              CELL_PAD_CTRL_DOWN);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::vector<std::set<u32>>{},
                              CELL_PAD_CTRL_LEFT);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::vector<std::set<u32>>{},
                              CELL_PAD_CTRL_RIGHT);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::vector<std::set<u32>>{},
                              CELL_PAD_CTRL_CROSS);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::vector<std::set<u32>>{},
                              CELL_PAD_CTRL_SQUARE);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::vector<std::set<u32>>{},
                              CELL_PAD_CTRL_CIRCLE);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::vector<std::set<u32>>{},
                              CELL_PAD_CTRL_TRIANGLE);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::vector<std::set<u32>>{},
                              CELL_PAD_CTRL_L1);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::vector<std::set<u32>>{},
                              CELL_PAD_CTRL_L2);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::vector<std::set<u32>>{},
                              CELL_PAD_CTRL_L3);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::vector<std::set<u32>>{},
                              CELL_PAD_CTRL_R1);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::vector<std::set<u32>>{},
                              CELL_PAD_CTRL_R2);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::vector<std::set<u32>>{},
                              CELL_PAD_CTRL_R3);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::vector<std::set<u32>>{},
                              CELL_PAD_CTRL_START);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::vector<std::set<u32>>{},
                              CELL_PAD_CTRL_SELECT);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::vector<std::set<u32>>{},
                              CELL_PAD_CTRL_PS);

  pad->m_sticks[0] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X, {}, {});
  pad->m_sticks[1] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y, {}, {});
  pad->m_sticks[2] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X, {}, {});
  pad->m_sticks[3] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y, {}, {});

  pad->m_sensors[0] =
      AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_X, 0, 0, 0, DEFAULT_MOTION_X);
  pad->m_sensors[1] =
      AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_Y, 0, 0, 0, DEFAULT_MOTION_Y);
  pad->m_sensors[2] =
      AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_Z, 0, 0, 0, DEFAULT_MOTION_Z);
  pad->m_sensors[3] =
      AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_G, 0, 0, 0, DEFAULT_MOTION_G);

  pad->m_vibrate_motors[0] = VibrateMotor(true);
  pad->m_vibrate_motors[1] = VibrateMotor(false);

  if (pad->m_player_id < static_cast<u32>(kMaxVirtualPads)) {
    std::lock_guard lock(g_virtual_pad_mutex);
    g_virtual_pads[pad->m_player_id] = pad;
  }
  return true;
}

// Sole purpose is calling the inherited PadHandlerBase::convert_stick_values
// (deadzone/anti-deadzone/squircle math, identical to what every real
// handler - evdev, ds4, xinput... - applies) without duplicating that math
// here. Never bound to a device, never driven by pad_thread; stateless.
static virtual_pad_handler g_stick_math_helper;

// leftStickX/Y, rightStickX/Y arrive already rescaled by the Kotlin side to
// PS3-native byte range (0-255, center ~128, low=up/left) via
// `axis*127+128` - see RPCSXActivity.kt. convert_stick_values expects signed
// input in (-255..255) with +Y meaning UP (opposite of the PS3-native
// low=up convention), matching how every desktop handler derives its own
// signed stick_val from up/down key combos before calling it - so X is
// recovered with the same sign as the wire value, Y is negated going in,
// then flipped back out (255-y_out) on the way out, exactly mirroring
// PadHandler.cpp's own get_extended_info (255-ly / 255-ry) for every other
// handler. Do not change one side without the other.
static s32 toSignedAxis(int wireValue) {
  return std::clamp((wireValue - 128) * 2, -255, 255);
}

static bool setVirtualPadData(int playerIndex, int digital1, int digital2,
                              int leftStickX, int leftStickY, int rightStickX,
                              int rightStickY, int leftTrigger = -1,
                              int rightTrigger = -1) {
  if (playerIndex < 0 || playerIndex >= kMaxVirtualPads ||
      playerIndex >= static_cast<int>(g_cfg_input.player.size())) {
    return false;
  }

  auto pad = [&] {
    std::shared_ptr<Pad> result;
    std::lock_guard lock(g_virtual_pad_mutex);
    result = g_virtual_pads[playerIndex];
    if (result == nullptr) {
      result = std::make_shared<Pad>(pad_handler::null, static_cast<u32>(playerIndex), CELL_PAD_STATUS_CONNECTED, 0, CELL_PAD_DEV_TYPE_STANDARD);
      initVirtualPad(result);
    }
    return result;
  }();

  const cfg_pad &cfg = g_cfg_input.player[playerIndex]->config;

  for (auto &btn : pad->m_buttons) {
    if (btn.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL1) {
      btn.m_pressed = (digital1 & btn.m_outKeyCode) != 0;
      btn.m_value = btn.m_pressed ? 255 : 0;

      if (playerIndex == 0 && btn.m_outKeyCode == CELL_PAD_CTRL_PS &&
          btn.m_pressed) {
        if (auto padThread = pad::get_pad_thread(true)) {
          padThread->open_home_menu();
        }
      }

    } else if (btn.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL2) {
      // L2/R2 double as analog triggers (CELL_PAD_CAPABILITY_PRESS_MODE,
      // see initVirtualPad). leftTrigger/rightTrigger < 0 means the caller
      // has no analog axis for this trigger (e.g. an older APK build still
      // calling the pre-trigger overlayPadData/multiPadData overload) -
      // fall back to the pre-existing digital-only 0/255 behavior so that
      // case keeps working exactly as before.
      const bool isL2 = btn.m_outKeyCode == CELL_PAD_CTRL_L2;
      const bool isR2 = btn.m_outKeyCode == CELL_PAD_CTRL_R2;
      if (isL2 && leftTrigger >= 0) {
        const u16 raw = static_cast<u16>(std::clamp(leftTrigger, 0, 255));
        btn.m_pressed = raw > cfg.ltriggerthreshold;
        btn.m_value = btn.m_pressed
                          ? g_stick_math_helper.normalize_trigger(
                                raw, cfg.ltriggerthreshold)
                          : 0;
      } else if (isR2 && rightTrigger >= 0) {
        const u16 raw = static_cast<u16>(std::clamp(rightTrigger, 0, 255));
        btn.m_pressed = raw > cfg.rtriggerthreshold;
        btn.m_value = btn.m_pressed
                          ? g_stick_math_helper.normalize_trigger(
                                raw, cfg.rtriggerthreshold)
                          : 0;
      } else {
        btn.m_pressed = (digital2 & btn.m_outKeyCode) != 0;
        btn.m_value = btn.m_pressed ? 255 : 0;
      }
    }
  }

  u16 lx, ly, rx, ry;
  g_stick_math_helper.convert_stick_values(
      lx, ly, toSignedAxis(leftStickX), -toSignedAxis(leftStickY),
      cfg.lstickdeadzone, cfg.lstick_anti_deadzone, cfg.lpadsquircling);
  g_stick_math_helper.convert_stick_values(
      rx, ry, toSignedAxis(rightStickX), -toSignedAxis(rightStickY),
      cfg.rstickdeadzone, cfg.rstick_anti_deadzone, cfg.rpadsquircling);

  pad->m_sticks[0].m_value = lx;
  pad->m_sticks[1].m_value = 255 - ly;
  pad->m_sticks[2].m_value = rx;
  pad->m_sticks[3].m_value = 255 - ry;
  return true;
}

// leftTrigger/rightTrigger are analog L2/R2 axis values (0-255), or -1 if
// the caller has none to offer (see setVirtualPadData's own fallback note).
extern "C" bool _rpcsx_overlayPadData(int digital1, int digital2,
                                      int leftStickX, int leftStickY,
                                      int rightStickX, int rightStickY,
                                      int leftTrigger, int rightTrigger) {
  return setVirtualPadData(0, digital1, digital2, leftStickX, leftStickY,
                           rightStickX, rightStickY, leftTrigger,
                           rightTrigger);
}

// Extended version of _rpcsx_overlayPadData that targets one of up to
// kMaxVirtualPads player slots, so multiple physical/on-screen controllers
// detected on the Android side can each drive a distinct PS3 pad.
extern "C" bool _rpcsx_multiPadData(int playerIndex, int digital1, int digital2,
                                    int leftStickX, int leftStickY,
                                    int rightStickX, int rightStickY,
                                    int leftTrigger, int rightTrigger) {
  return setVirtualPadData(playerIndex, digital1, digital2, leftStickX,
                           leftStickY, rightStickX, rightStickY, leftTrigger,
                           rightTrigger);
}

extern "C" int _rpcsx_getMaxVirtualPads() { return kMaxVirtualPads; }

// Backend rumble: the game writes motor strength into the virtual pad's
// vibrate motors (cellPadSetActDirect). The Android side has no rumble output
// of its own, so it polls this and drives the physical controller / phone
// vibrator. Returns (large << 8) | small, each 0-255; 0 when idle or no pad.
extern "C" int _rpcsx_getPadVibration(int playerIndex) {
  if (playerIndex < 0 || playerIndex >= kMaxVirtualPads ||
      playerIndex >= static_cast<int>(g_cfg_input.player.size())) {
    return 0;
  }

  std::shared_ptr<Pad> pad;
  {
    std::lock_guard lock(g_virtual_pad_mutex);
    pad = g_virtual_pads[playerIndex];
  }

  if (pad == nullptr) {
    return 0;
  }

  // Same call every real handler makes right before writing to hardware
  // (e.g. ds4_pad_handler::apply_pad_data) - applies multiplier_vibration_
  // motor_large/_small, switch_vibration_motors and vibration_threshold from
  // cfg_pad instead of forwarding the game's raw motor values untouched.
  const cfg_pad &cfg = g_cfg_input.player[playerIndex]->config;
  const int large = cfg.get_large_motor_speed(pad->m_vibrate_motors);
  const int small = cfg.get_small_motor_speed(pad->m_vibrate_motors);
  return ((large & 0xff) << 8) | (small & 0xff);
}

// Read-only poll of a virtual pad's current (already deadzone/squircle-
// processed, see setVirtualPadData) stick values, for a live UI preview -
// e.g. the pad tuning screen's stick-position canvas. Packs all 4 axes (each
// 0-255, PS3-native convention) into one int: (lx<<24)|(ly<<16)|(rx<<8)|ry.
extern "C" int _rpcsx_getStickPosition(int playerIndex) {
  if (playerIndex < 0 || playerIndex >= kMaxVirtualPads) {
    return 0;
  }

  std::shared_ptr<Pad> pad;
  {
    std::lock_guard lock(g_virtual_pad_mutex);
    pad = g_virtual_pads[playerIndex];
  }

  if (pad == nullptr) {
    return 0;
  }

  const int lx = pad->m_sticks[0].m_value & 0xff;
  const int ly = pad->m_sticks[1].m_value & 0xff;
  const int rx = pad->m_sticks[2].m_value & 0xff;
  const int ry = pad->m_sticks[3].m_value & 0xff;
  return (lx << 24) | (ly << 16) | (rx << 8) | ry;
}

// Name of the first Vulkan physical device, or empty if Vulkan is unusable.
// Emu.Init() requires a non-empty adapter name whenever the default renderer
// is Vulkan (System.cpp ensure), so this must run before it.
static std::string firstVulkanAdapter() {
  if (!vkCreateInstance || !vkEnumeratePhysicalDevices ||
      !vkGetPhysicalDeviceProperties || !vkDestroyInstance) {
    return {};
  }

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "RPCSX";
  app.apiVersion = VK_API_VERSION_1_0;

  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &app;

  VkInstance instance = VK_NULL_HANDLE;
  if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS || !instance) {
    rpcsx_android.error("firstVulkanAdapter: vkCreateInstance failed");
    return {};
  }

  std::string name;
  u32 count = 0;
  if (vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS &&
      count != 0) {
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) ==
        VK_SUCCESS) {
      VkPhysicalDeviceProperties props{};
      vkGetPhysicalDeviceProperties(devices[0], &props);
      name = props.deviceName;
    }
  }

  vkDestroyInstance(instance, nullptr);
  return name;
}

extern "C" bool _rpcsx_initialize(std::string_view rootDir,
                                  std::string_view user) {
  auto rootDirStr = fix_dir_path(std::string(rootDir));

  if (g_android_executable_dir != rootDirStr) {
    g_android_executable_dir = rootDirStr;
    g_android_config_dir = rootDirStr + "config/";
    g_android_cache_dir = rootDirStr + "cache/";

    std::filesystem::create_directories(g_android_config_dir);
    std::error_code ec;
    // std::filesystem::remove_all(g_android_cache_dir, ec);
    std::filesystem::create_directories(g_android_cache_dir);
  }

  if (g_initialized) {
    return true;
  }

  g_initialized = true;

  if (int r = libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY,
                                nullptr);
      r != 0) {
    rpcsx_android.warning(
        "libusb_set_option(LIBUSB_OPTION_NO_DEVICE_DISCOVERY) -> %d", r);
  }

  // Initialize thread pool finalizer // ???
  static_cast<void>(named_thread("", [](int) {}));

  static std::unique_ptr<logs::listener> log_file;
  {
    // Check free space
    fs::device_stat stats{};
    if (!fs::statfs(fs::get_cache_dir(), stats) ||
        stats.avail_free < 128 * 1024 * 1024) {
      std::fprintf(stderr, "Not enough free space for logs (%f KB)",
                   stats.avail_free / 1000000.);
    }

    // preserve old log file
    if (std::filesystem::exists(fs::get_log_dir() + "RPCSX.log")) {
      std::error_code ec;
      std::filesystem::remove(fs::get_log_dir() + "RPCSX.old.log", ec);
      std::filesystem::rename(fs::get_log_dir() + "RPCSX.log",
                              fs::get_log_dir() + "RPCSX.old.log", ec);
    }

    // Limit log size to ~25% of free space
    log_file = logs::make_file_listener(fs::get_log_dir() + "RPCSX.log",
                                        stats.avail_free / 4);
  }

  logs::stored_message ver{rpcsx_android.always()};
  ver.text = fmt::format("RPCSX-ps3-android v%s", rpcs3::get_version().to_string());

  // Write System information
  logs::stored_message sys{rpcsx_android.always()};
  sys.text = utils::get_system_info();

  // Write OS version
  logs::stored_message os{rpcsx_android.always()};
  os.text = utils::get_OS_version_string();

  // Write current time
  logs::stored_message time{rpcsx_android.always()};
  time.text = fmt::format("Current Time: %s", std::chrono::system_clock::now());

  logs::set_init(
      {std::move(ver), std::move(sys), std::move(os), std::move(time)});

  auto set_rlim = [](int resource, std::uint64_t limit) {
    rlimit64 rlim{};
    if (getrlimit64(resource, &rlim) != 0) {
      rpcsx_android.error("failed to get rlimit for %d", resource);
      return;
    }

    rlim.rlim_cur = std::min<std::size_t>(rlim.rlim_max, limit);
    rpcsx_android.error("rlimit[%d] = %u (requested %u, max %u)", resource,
                        rlim.rlim_cur, limit, rlim.rlim_max);

    if (setrlimit64(resource, &rlim) != 0) {
      rpcsx_android.error("failed to set rlimit for %d", resource);
      return;
    }
  };

  set_rlim(RLIMIT_MEMLOCK, RLIM_INFINITY);
  set_rlim(RLIMIT_NOFILE, RLIM_INFINITY);
  set_rlim(RLIMIT_STACK, 256 * 1024 * 1024);
  set_rlim(RLIMIT_AS, RLIM_INFINITY);

  // Resolve the Vulkan entry points from the system libvulkan unless a
  // custom driver was already injected via _rpcsx_setCustomDriver.
  vk::ensure_dynamic_symbols();

  virtual_pad_handler::set_on_connect_cb(initVirtualPad);
  setupCallbacks();
  Emu.SetHasGui(false);
  Emu.SetUsr(std::string(user));

  // fixup_settings() inside Emu.Init() validates the configured renderer
  // against this set - empty in a non-Qt build unless we populate it - and
  // otherwise force-resets it to the default renderer (Null when unset).
  // The SaveSettings() below would then persist that reset, wiping whatever
  // the user picked in the settings UI on every app start. A Vulkan default
  // additionally requires a concrete adapter name (System.cpp ensure).
  std::set<video_renderer> supportedRenderers{video_renderer::null};
  if (std::string adapter = firstVulkanAdapter(); !adapter.empty()) {
    rpcsx_android.notice("Default GPU: '%s'", adapter);
    supportedRenderers.insert(video_renderer::vulkan);
    Emu.SetDefaultRenderer(video_renderer::vulkan);
    Emu.SetDefaultGraphicsAdapter(std::move(adapter));
  } else {
    rpcsx_android.error("No Vulkan device found, falling back to Null renderer");
  }
  Emu.SetSupportedRenderers(std::move(supportedRenderers));

  Emu.Init();

  static_assert(kMaxVirtualPads == 4);
  if (!g_cfg_input.load("", g_cfg_input_configs.default_config)) {
    g_cfg_input.player1.handler.set(pad_handler::virtual_pad);
    g_cfg_input.player1.device.from_string("Virtual");
    g_cfg_input.player2.handler.set(pad_handler::virtual_pad);
    g_cfg_input.player2.device.from_string("Virtual");
    g_cfg_input.player3.handler.set(pad_handler::virtual_pad);
    g_cfg_input.player3.device.from_string("Virtual");
    g_cfg_input.player4.handler.set(pad_handler::virtual_pad);
    g_cfg_input.player4.device.from_string("Virtual");
    g_cfg_input.save("", g_cfg_input_configs.default_config);
  }

  g_cfg.core.llvm_cpu.from_string("oryon-1");
  g_cfg.core.llvm_threads.from_string("1");

  // The upstream default (async_with_interpreter) stalls the Adreno driver
  // precompiling interpreter variants, and the async recompiler's worker
  // threads also hang on this driver (pending investigation) - the legacy
  // recompiler is the only mode that currently works here.
  g_cfg.video.shadermode.set(shader_mode::recompiler);

  // Audio output on Android goes through cubeb (OpenSL ES / AAudio).
  // WITHOUT_OPENAL only disables cellMic microphone capture - it does not
  // affect game audio. However, configs persisted by older builds may carry
  // "Renderer: Null", which leaves the emulator silent even though cubeb
  // works fine; snap those back to cubeb here.
  if (g_cfg.audio.renderer.get() == audio_renderer::null) {
    rpcsx_android.warning(
        "Audio renderer was Null (stale config?), resetting to Cubeb");
    g_cfg.audio.renderer.set(audio_renderer::cubeb);
  }

  // Persist the (possibly fixed-up) startup config; this also rewrites a
  // YAML-format config.json left by older builds or by the core's
  // first-boot default dump as JSON.
  save_global_config();
  return true;
}

extern "C" bool _rpcsx_processCompilationQueue(JNIEnv *env) {
  g_compilationQueue.process(env);
  return true;
}

extern "C" bool _rpcsx_startMainThreadProcessor(JNIEnv *env) {
  g_mainThreadProcessor.process(env);
  return true;
}

extern "C" bool _rpcsx_collectGameInfo(JNIEnv *env, std::string_view rootDir,
                                       long progressId) {

  if (std::filesystem::is_regular_file(g_cfg_vfs.get_dev_flash() +
                                       "/vsh/module/vsh.self")) {
    sendVshBootable(env, progressId);
  }

  collectGameInfo(env, progressId, {std::string(rootDir)});
  return true;
}

extern "C" void _rpcsx_shutdown() { Emu.Kill(); }

extern "C" int _rpcsx_boot(std::string_view path_, std::string_view config_path_) {
  Emu.SetForceBoot(true);
  std::string path = std::string(path_);
  while (path.ends_with('/')) {
    path.pop_back();
  }

  std::string config_path = std::string(config_path_);
  cfg_mode mode = config_path.empty() ? cfg_mode::custom : cfg_mode::custom_selection;
  int result = static_cast<int>(Emu.BootGame(path, "", false, mode, config_path));
  rpcsx_android.error("_rpcsx_boot: BootGame returned %d for path %s (config=%s)", result, path.c_str(), config_path.c_str());
  return result;
}

// Best-effort real filesystem path behind an open fd. Android SAF fds that are
// backed by a real file (internal storage, SD card, Download raw files) expose
// it through /proc/self/fd/<n>; genuinely path-less sources (cloud providers,
// MediaStore streams) do not resolve to a regular file and return empty.
static std::string realPathForFd(int fd) {
  if (fd < 0) {
    return {};
  }
  char buf[4096];
  const std::string link = fmt::format("/proc/self/fd/%d", fd);
  const ssize_t n = ::readlink(link.c_str(), buf, sizeof(buf) - 1);
  if (n <= 0) {
    return {};
  }
  buf[n] = '\0';
  std::string path(buf);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return {};
  }
  return path;
}

// Boots an ISO by resolving the fd to its real filesystem path and mounting it
// path-based (load_iso(path) inside Emu::Load). This is the AetherSX2-style
// model: real paths + all-files access, which survives an APK reinstall (SAF
// content:// grants do not). Path-less sources are not supported. Ownership of
// `fd` transfers to native; it is closed here since the boot uses the path.
extern "C" int _rpcsx_bootIsoFd(int fd, std::string_view config_path_) {
  const std::string path = realPathForFd(fd);
  if (fd >= 0) {
    ::close(fd);
  }

  if (path.empty() || !is_iso_file(path)) {
    rpcsx_android.error("_rpcsx_bootIsoFd: fd %d has no real ISO path ('%s')",
                        fd, path.c_str());
    return static_cast<int>(game_boot_result::invalid_file_or_folder);
  }

  Emu.SetForceBoot(true);

  std::string config_path = std::string(config_path_);
  cfg_mode mode = config_path.empty() ? cfg_mode::custom : cfg_mode::custom_selection;
  int result = static_cast<int>(Emu.BootGame(path, "", false, mode, config_path));
  rpcsx_android.error("_rpcsx_bootIsoFd: BootGame('%s') returned %d (config=%s)",
                      path.c_str(), result, config_path.c_str());
  return result;
}

extern "C" int _rpcsx_getState() {
  return static_cast<int>(Emu.GetStatus(false));
}
extern "C" void _rpcsx_kill() { Emu.Kill(); }
extern "C" void _rpcsx_resume() { Emu.Resume(); }

extern "C" void _rpcsx_openHomeMenu() {
  if (auto padThread = pad::get_pad_thread(true)) {
    padThread->open_home_menu();
  }
}

extern "C" std::string _rpcsx_getTitleId() { return Emu.GetTitleID(); }

extern "C" bool _rpcsx_surfaceEvent(JNIEnv *env, jobject surface, jint event) {
  rpcsx_android.warning("surface event %p, %d", surface, event);

  if (event == 2) {
    auto prevWindow = g_native_window.exchange(nullptr);
    if (prevWindow != nullptr) {
      ANativeWindow_release(prevWindow);
    }

    // Relaxed access: the pad thread does not exist yet while the game is
    // still compiling PPU modules/shaders, nor after a failed boot - the
    // strict accessor's ensure() would abort the whole process right when
    // the surface goes away in those states.
    if (auto padThread = pad::get_pad_thread(true)) {
      padThread->open_home_menu();
    }

    Emu.Pause();
  } else {
    auto newWindow = ANativeWindow_fromSurface(env, surface);

    if (newWindow == nullptr) {
      rpcsx_android.fatal("returned native window is null, surface %p",
                          surface);
      return false;
    }

    auto prevWindow = g_native_window.exchange(newWindow);

    if (newWindow != prevWindow) {
      ANativeWindow_acquire(newWindow);

      if (prevWindow != nullptr) {
        ANativeWindow_release(prevWindow);
      }
    }

    if (event == 0 && Emu.IsPaused()) {
      Emu.Resume();
    }
  }

  return true;
}

extern "C" bool _rpcsx_usbDeviceEvent(int fd, int vendorId, int productId,
                                      int event) {
  rpcsx_android.warning(
      "usb device event %d fd: %d, vendorId: %d, productId: %d", event, fd,
      vendorId, productId);

  {
    std::lock_guard lock(g_android_usb_devices_mutex);

    if (event == 0) {
      g_android_usb_devices.push_back({
          .fd = int(fd),
          .vendorId = u16(vendorId),
          .productId = u16(productId),
      });
    } else {
      auto filter = [fd](auto device) { return device.fd == fd; };
      if (auto it = std::ranges::find_if(g_android_usb_devices, filter);
          it != g_android_usb_devices.end()) {
        g_android_usb_devices.erase(it);
      }
    }
  }

  {
    auto selectedHandler = g_cfg_input.player1.handler.get();
    std::string selectedDevice;

    std::map<pad_handler, std::pair<std::unique_ptr<PadHandlerBase>,
                                    std::vector<std::string>>>
        handlerToDevices;

    auto collectDevices = [&]<typename T>(T handler) {
      handler->Init();

      std::vector<std::string> devices;
      for (const auto &device : handler->list_devices()) {
        devices.push_back(device.name);
      }

      auto type = handler->m_type;

      handlerToDevices[type] = std::pair{
          std::move(handler),
          std::move(devices),
      };
    };

    collectDevices(std::make_unique<dualsense_pad_handler>());
    collectDevices(std::make_unique<ds4_pad_handler>());
    collectDevices(std::make_unique<ds3_pad_handler>());

    if (handlerToDevices[selectedHandler].second.empty()) {
      selectedHandler = pad_handler::null;
    }

    if (!handlerToDevices[pad_handler::dualsense].second.empty()) {
      selectedHandler = pad_handler::dualsense;
    } else if (!handlerToDevices[pad_handler::ds4].second.empty()) {
      selectedHandler = pad_handler::ds4;
    } else if (!handlerToDevices[pad_handler::ds3].second.empty()) {
      selectedHandler = pad_handler::ds3;
    }

    if (selectedHandler == pad_handler::null) {
      selectedHandler = pad_handler::virtual_pad;
    }

    if (selectedHandler != g_cfg_input.player1.handler.get()) {
      rpcsx_android.warning("install %s pad handler", selectedHandler);

      g_cfg_input.player1.handler.set(selectedHandler);

      if (selectedHandler == pad_handler::null) {
        g_cfg_input.player1.device.from_default();
      } else if (selectedHandler == pad_handler::virtual_pad) {
        g_cfg_input.player1.handler.set(pad_handler::virtual_pad);
        g_cfg_input.player1.device.from_string("Virtual");
      } else {
        g_cfg_input.player1.device.from_string(
            handlerToDevices[selectedHandler].second.front());
        handlerToDevices[selectedHandler].first->init_config(
            &g_cfg_input.player1.config);
        if (selectedHandler != pad_handler::virtual_pad) {
          std::lock_guard lock(g_virtual_pad_mutex);
          g_virtual_pads[0] = nullptr;
        }
      }

      g_cfg_input.save("", g_cfg_input_configs.default_config);

      if (!Emu.IsStopped()) {
        pad::reset(Emu.GetTitleID());
      }
    }
  }

  return true;
}

static bool installPup(JNIEnv *env, fs::file &&pup_f, jlong progressId) {
  Progress progress(env, progressId);

  pup_object pup(std::move(pup_f));
  AtExit atExit{[&] { pup.file().release_handle(); }};

  if (static_cast<pup_error>(pup) == pup_error::hash_mismatch) {
    rpcsx_android.fatal("installFw: invalid PUP");
    progress.failure("Selected file is not firmware update file");
    return false;
  }

  if (static_cast<pup_error>(pup) != pup_error::ok) {
    rpcsx_android.fatal("installFw: invalid PUP");
    progress.failure("Firmware update file is broken");
    return false;
  }

  fs::file update_files_f = pup.get_file(0x300);

  const usz update_files_size = update_files_f ? update_files_f.size() : 0;

  if (!update_files_size) {
    rpcsx_android.fatal("installFw: invalid PUP");
    progress.failure("Firmware update file is broken");
    return false;
  }

  tar_object update_files(update_files_f);

  auto update_filenames = update_files.get_filenames();
  update_filenames.erase(std::remove_if(update_filenames.begin(),
                                        update_filenames.end(),
                                        [](const std::string &s) {
                                          return !s.starts_with("dev_flash_");
                                        }),
                         update_filenames.end());

  if (update_filenames.empty()) {
    rpcsx_android.fatal("installFw: invalid PUP");
    progress.failure("Firmware update file is broken");
    return false;
  }

  std::string version_string;

  if (fs::file version = pup.get_file(0x100)) {
    version_string = version.to_string();
  }

  if (const usz version_pos = version_string.find('\n');
      version_pos != std::string::npos) {
    version_string.erase(version_pos);
  }

  if (version_string.empty()) {
    rpcsx_android.fatal("installFw: invalid PUP");
    progress.failure("Firmware update file is broken");
    return false;
  }

  sendVshBootable(env, progressId);

  jlong processed = 0;
  for (const auto &update_filename : update_filenames) {
    auto update_file_stream = update_files.get_file(update_filename);

    if (update_file_stream->m_file_handler) {
      // Forcefully read all the data
      update_file_stream->m_file_handler->handle_file_op(
          *update_file_stream, 0, update_file_stream->get_size(umax), nullptr);
    }

    fs::file update_file = fs::make_stream(std::move(update_file_stream->data));

    SCEDecrypter self_dec(update_file);
    self_dec.LoadHeaders();
    self_dec.LoadMetadata(SCEPKG_ERK, SCEPKG_RIV);
    self_dec.DecryptData();

    auto dev_flash_tar_f = self_dec.MakeFile();

    if (dev_flash_tar_f.size() < 3) {
      rpcsx_android.error(
          "Firmware installation failed: Firmware could not be decompressed");

      progress.failure("Firmware update file could not be decompressed");
      return false;
    }

    tar_object dev_flash_tar(dev_flash_tar_f[2]);

    if (!dev_flash_tar.extract()) {

      rpcsx_android.error("Error while installing firmware: TAR contents are "
                          "invalid. (package=%s)",
                          update_filename);

      progress.failure(fmt::format("TAR contents are invalid (package=%s)",
                                   update_filename));
      return false;
    }

    if (!progress.report(processed++, update_filenames.size())) {
      // Installation was cancelled
      return false;
    }
  }

  sendFirmwareInstalled(env, utils::get_firmware_version());

  // NOTE: do NOT queue vsh.self for PPU precompilation here. That runs
  // ppu_register_range on the compilation-queue thread, which asserts inside
  // ensure(vm::page_protect(...)) because the PS3 vm is not initialized
  // outside a real game boot - an uncatchable SIGTRAP that crashed the app
  // right after an otherwise successful firmware install (PPUThread.cpp:779).
  // The firmware is fully installed above; its modules are compiled lazily on
  // first boot instead. Mark the progress done ourselves since the queue,
  // which used to close it, no longer runs.
  progress.success(0);
  return true;
}

static bool installPkg(JNIEnv *env, fs::file &&file, jlong progressId) {
  Progress progress(env, progressId);

  std::deque<package_reader> readers;
  std::deque<std::string> bootable_paths;
  readers.emplace_back("dummy.pkg", std::move(file));

  AtExit atExit{[&] {
    for (auto &reader : readers) {
      reader.file().release_handle();
    }
  }};

  package_install_result result = {};
  named_thread worker("PKG Installer", [&readers, &result, &bootable_paths] {
    result = package_reader::extract_data(readers, bootable_paths);
    return result.error == package_install_result::error_type::no_error;
  });

  for (auto &reader : readers) {
    // A patch/update PKG's own PARAM.SFO is normally BOOTABLE=0, so
    // fetchGameInfo simply returns nullopt for it here - nothing sent, no
    // placeholder-stealing stub entry. The version bump it installs is
    // picked up next time the real game (installed copy, ISO or folder) is
    // scanned, via fetchGameInfo's dev_hdd0/game/<id>/PARAM.SFO lookup.
    if (auto gameInfo = fetchGameInfo(reader.get_psf())) {
      sendGameInfo(env, progressId, {{*gameInfo}});
    }
  }

  const jlong maxProgress = 10000;

  while (true) {
    std::uint64_t totalProgress = 0;
    for (auto &reader : readers) {
      if (result.error != package_install_result::error_type::no_error) {
        progress.failure("Installation failed");
        for (package_reader &reader : readers) {
          reader.abort_extract();
        }
        return false;
      }

      totalProgress += reader.get_progress(maxProgress);
    }

    if (totalProgress == maxProgress * readers.size()) {
      break;
    }

    totalProgress /= readers.size();

    if (!progress.report(totalProgress, maxProgress)) {
      for (package_reader &reader : readers) {
        reader.abort_extract();
      }

      return false;
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));
  }

  if (worker()) {
    auto paths = std::vector(bootable_paths.begin(), bootable_paths.end());
    collectGameInfo(env, -1, paths);
  }

  // Installing a PKG only extracts it - do NOT precompile here. Precompiling
  // from the compile-queue thread runs ppu_register_range without a booted vm
  // and aborts (ensure(vm::page_protect(...))), which crashed the app right
  // after a successful install ("installing modules"). PPU modules are built
  // lazily on first boot, where Emu::Load sets the vm up correctly.
  progress.success(0);
  return true;
}

static bool installEdat(JNIEnv *env, fs::file &&file, jlong progressId,
                        std::string_view rootPath = {}) {
  Progress progress(env, progressId);

  // Java owns this fd; don't let fs::file's destructor close it (fdsan).
  AtExit atExit{[&] { file.release_handle(); }};

  NPD_HEADER npdHeader;
  if (!file.read(npdHeader)) {
    progress.failure("Invalid EDAT file");
    return false;
  }

  if (!rootPath.empty()) {
    auto ebootPath = locateEbootPath(rootPath);
    auto sfoPath = locateParamSfoPath(rootPath);

    if (sfoPath.empty()) {
      progress.failure("Game is broken: PARAM.SFO not found");
      return false;
    }

    auto psf = psf::load_object(sfoPath);
    auto contentId = psf::get_string(psf, "CONTENT_ID");

    if (contentId != npdHeader.content_id) {
      progress.failure(fmt::format("File cannot be used for this game. EDAT "
                                   "content ID missmatch %s vs %s",
                                   contentId, npdHeader.content_id));
      return false;
    }
  }

  const auto licenseFile =
      fmt::format("%shome/%s/exdata/%s.edat", rpcs3::utils::get_hdd0_dir(),
                  Emu.GetUsr(), npdHeader.content_id);

  file.seek(0);

  std::vector<std::uint8_t> bytes(file.size());
  if (!file.read(bytes)) {
    progress.failure("Failed to read key");
    return false;
  }

  if (!fs::write_file(licenseFile, fs::open_mode::create + fs::open_mode::trunc,
                      bytes)) {
    progress.failure(fmt::format("Failed to write EDAT to %s", licenseFile));
    return false;
  }

  auto root = std::string(rootPath);

  if (root.empty()) {
    root = rpcs3::utils::get_hdd0_dir() + "game";
  }

  collectGameInfo(env, progressId, {std::move(root)});
  return true;
}

static bool installRap(JNIEnv *env, fs::file &&file, jlong progressId,
                       std::string_view rootPath) {
  Progress progress(env, progressId);

  // Java owns this fd; don't let fs::file's destructor close it (fdsan).
  AtExit atExit{[&] { file.release_handle(); }};

  auto ebootPath = locateEbootPath(rootPath);

  std::vector<std::uint8_t> bytes;
  if (!file.read(bytes, 16)) {
    progress.failure("Failed to read key");
    return false;
  }

  SelfAdditionalInfo info;
  decrypt_self(fs::file(ebootPath), nullptr, &info);

  auto npd = [&]() -> NPD_HEADER * {
    for (auto &supplemental : info.supplemental_hdr) {
      if (supplemental.type == 3) {
        return &supplemental.PS3_npdrm_header.npd;
      }
    }

    return nullptr;
  }();

  if (npd == nullptr) {
    progress.failure("Failed to fetch NPDRM of SELF");
    return false;
  }

  const auto licenseFile =
      fmt::format("%shome/%s/exdata/%s.rap", rpcs3::utils::get_hdd0_dir(),
                  Emu.GetUsr(), npd->content_id);

  if (!fs::write_file(licenseFile, fs::open_mode::create + fs::open_mode::trunc,
                      bytes)) {
    progress.failure(fmt::format("Failed to write key to %s", licenseFile));
    return false;
  }

  if (!decrypt_self(fs::file(ebootPath))) {
    progress.failure("Provided key is invalid for selected game");
    fs::remove_file(licenseFile);
    return false;
  }

  collectGameInfo(env, -1, {std::string(rootPath)});
  // No precompile: the module builds lazily on first boot (see installPkg).
  (void)ebootPath;
  progress.success(0);
  return true;
}

// Shared body: given a constructed iso_archive, extract the sidecars the UI
// needs to render the game entry, then register the GameInfo. Used by both
// the SAF-fd install path and the folder-scan path.
static bool registerIsoArchive(JNIEnv *env, jlong progressId,
                               iso_archive &archive,
                               std::string_view sourceUri, u64 totalSize) {
  Progress progress(env, progressId);
  progress.report(0, totalSize);

  const auto sfo = archive.open_psf("PS3_GAME/PARAM.SFO");
  const auto title_id = psf::get_string(sfo, "TITLE_ID");

  if (title_id.empty()) {
    rpcsx_android.error(
        "installIso: no TITLE_ID in PS3_GAME/PARAM.SFO (sfo entries: %zu)",
        sfo.size());
    progress.failure("Failed to fetch TITLE_ID from PARAM.SFO in ISO");
    return false;
  }

  rpcsx_android.notice("installIso: installing '%s'", title_id);

  const std::string gamesDir = fs::get_config_dir() + "games/";
  std::error_code ec;
  std::filesystem::create_directories(gamesDir, ec);

  const std::filesystem::path destinationPath =
      gamesDir + std::string(title_id);
  std::filesystem::create_directories(destinationPath / "PS3_GAME", ec);

  for (const char *sidecar : {"PS3_GAME/PARAM.SFO", "PS3_GAME/ICON0.PNG",
                              "PS3_GAME/ICON1.PAM"}) {
    if (!archive.is_file(sidecar)) {
      continue;
    }

    fs::file src;
    src.reset(archive.open(sidecar));

    if (!src) {
      continue;
    }

    if (!fs::write_file((destinationPath / sidecar).string(),
                        fs::open_mode::create + fs::open_mode::trunc,
                        src.to_vector<std::uint8_t>())) {
      progress.failure(fmt::format("Failed to write %s",
                                   (destinationPath / sidecar).string()));
      return false;
    }
  }

  if (!std::filesystem::is_regular_file(destinationPath / "PS3_GAME/PARAM.SFO")) {
    progress.failure("Failed to extract PARAM.SFO from ISO");
    return false;
  }

  auto localPsf = psf::load_object(destinationPath / "PS3_GAME/PARAM.SFO");
  if (auto gameInfo = fetchGameInfo(localPsf, destinationPath)) {
    gameInfo->sourceUri = std::string(sourceUri);
    sendGameInfo(env, progressId, {{*gameInfo}});
  }

  progress.success(totalSize);
  return true;
}

static bool registerIsoByPath(JNIEnv *env, jlong progressId,
                              const std::string &isoPath);

// On-the-fly ISO support (Loader/ISO.h): nothing is extracted besides tiny
// sidecar files for the game list UI (PARAM.SFO/ICON0.PNG/ICON1.PAM under
// games/<TITLE_ID>/PS3_GAME/). The .iso itself is never copied; the game is
// booted directly from its real path - Emu.BootGame() mounts it via
// load_iso(path).
//
// Path-based model (AetherSX2-style: real paths + all-files access). The SAF
// source is resolved to a real filesystem path - preferentially through the
// fd's /proc/self/fd link, falling back to resolveTreeUriToPath - and the ISO
// is registered by that path. A stored /storage/ path boots directly and
// survives an APK reinstall, unlike a SAF content:// grant which is revoked.
// Genuinely path-less sources (cloud providers, MediaStore streams) are not
// supported: the user must place the .iso on internal/SD storage.
static bool installIso(JNIEnv *env, fs::file &&file, jlong progressId, std::string_view sourceUri) {
  Progress progress(env, progressId);

  // The Java side owns this fd (ParcelFileDescriptor). Prevent our fs::file
  // destructor from closing it - otherwise Kotlin's descriptor.close() double
  // closes and fdsan aborts the process.
  AtExit atExit{[&] { file.release_handle(); }};

  std::string realPath = realPathForFd(file.get_handle());

  if (realPath.empty()) {
    if (std::string resolved = resolveTreeUriToPath(env, sourceUri);
        !resolved.empty()) {
      std::error_code ec;
      if (std::filesystem::is_regular_file(resolved, ec)) {
        realPath = std::move(resolved);
      }
    }
  }

  if (realPath.empty()) {
    rpcsx_android.error("installIso: no real path for source '%s'",
                        std::string(sourceUri));
    progress.failure("ISO must be on internal storage or an SD card. Cloud or "
                     "Downloads-provider files have no usable path - copy the "
                     ".iso to local storage and add it from there.");
    return false;
  }

  rpcsx_android.notice("installIso: registering by real path '%s'", realPath);

  if (!registerIsoByPath(env, progressId, realPath)) {
    progress.failure("Failed to read ISO");
    return false;
  }

  return true;
}

// Register an ISO located on the local filesystem by real path. Used by the
// "Add ISO directory" folder-scan flow so the game boots directly from the
// user-chosen file (no sourceUri fallback, no copying).
static bool registerIsoByPath(JNIEnv *env, jlong progressId,
                              const std::string &isoPath) {
  if (!fs::is_file(isoPath) || !is_iso_file(isoPath)) {
    return false;
  }

  iso_archive archive(isoPath);

  u64 totalSize = 0;
  if (fs::file f(isoPath); f) {
    totalSize = f.size();
  }

  // Empty sourceUri means: boot directly from GameInfo.path, which we set to
  // the real ISO path via fetchGameInfo -> findIsoInDir. But that only works
  // when the sidecar dir contains a copy of the .iso, which we skip on
  // purpose. Instead, register the ISO's real filesystem path as the source
  // URI (RPCSXActivity's boot path handles a plain path fine).
  return registerIsoArchive(env, progressId, archive, isoPath, totalSize);
}

// Walk `rootDir` recursively looking for .iso files and register each. The
// user-facing "Add ISO directory" flow calls this via _rpcsx_collectIsoInfo.
static void collectIsoInfo(JNIEnv *env, jlong progressId,
                           const std::string &rootDir) {
  std::error_code ec;
  std::vector<std::string> isoPaths;

  if (std::filesystem::is_regular_file(rootDir, ec)) {
    // Allow a single .iso file to be passed directly.
    isoPaths.push_back(rootDir);
  } else if (std::filesystem::is_directory(rootDir, ec)) {
    std::vector<std::filesystem::path> workList;
    workList.push_back(rootDir);
    while (!workList.empty()) {
      auto dir = std::move(workList.back());
      workList.pop_back();

      for (auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_directory(ec)) {
          workList.push_back(entry.path());
          continue;
        }
        if (!entry.is_regular_file(ec)) {
          continue;
        }
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (ext == ".iso") {
          isoPaths.push_back(entry.path().string());
        }
      }
    }
  }

  rpcsx_android.notice("collectIsoInfo: found %zu iso file(s) under '%s'",
                       isoPaths.size(), rootDir);

  Progress progress(env, progressId);
  progress.report(0, isoPaths.size());

  std::size_t processed = 0;
  for (const auto &iso : isoPaths) {
    if (!registerIsoByPath(env, /*progressId=*/-1, iso)) {
      rpcsx_android.error("collectIsoInfo: failed to register '%s'", iso);
    }
    progress.report(++processed, isoPaths.size());
  }

  progress.success(processed);
}

// Android-only glue: resolves a Storage Access Framework DocumentsProvider
// tree URI for the primary/secondary external storage volume to a real
// filesystem path, e.g.
//   content://com.android.externalstorage.documents/tree/primary%3AGames
//     -> /storage/emulated/0/Games
//   content://com.android.externalstorage.documents/tree/1234-5678%3APS3
//     -> /storage/1234-5678/PS3
// Returns an empty string if the URI is not a resolvable local-storage tree
// (cloud providers, USB OTG document providers, etc.).
static std::string percentDecode(std::string_view input) {
  std::string decoded;
  decoded.reserve(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    char c = input[i];
    if (c == '%' && i + 2 < input.size()) {
      auto hexToNibble = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return -1;
      };
      int hi = hexToNibble(input[i + 1]);
      int lo = hexToNibble(input[i + 2]);
      if (hi >= 0 && lo >= 0) {
        decoded.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    decoded.push_back(c);
  }
  return decoded;
}

static std::string getPrimaryStoragePath(JNIEnv *env) {
  if (env) {
    if (jclass envClass = env->FindClass("android/os/Environment")) {
      if (jmethodID getExtDir = env->GetStaticMethodID(envClass, "getExternalStorageDirectory", "()Ljava/io/File;")) {
        if (jobject fileObj = env->CallStaticObjectMethod(envClass, getExtDir)) {
          if (jclass fileClass = env->FindClass("java/io/File")) {
            if (jmethodID getAbsPath = env->GetMethodID(fileClass, "getAbsolutePath", "()Ljava/lang/String;")) {
              if (jstring pathStr = static_cast<jstring>(env->CallObjectMethod(fileObj, getAbsPath))) {
                const char *chars = env->GetStringUTFChars(pathStr, nullptr);
                std::string path = chars ? chars : "";
                if (chars) env->ReleaseStringUTFChars(pathStr, chars);
                if (!path.empty()) return path;
              }
            }
          }
        }
      }
    }
  }
  return "/storage/emulated/0";
}

static std::string getDownloadsDirectoryPath(JNIEnv *env) {
  if (env) {
    if (jclass envClass = env->FindClass("android/os/Environment")) {
      if (jfieldID dirDownloadsField = env->GetStaticFieldID(envClass, "DIRECTORY_DOWNLOADS", "Ljava/lang/String;")) {
        if (jstring downloadsType = static_cast<jstring>(env->GetStaticObjectField(envClass, dirDownloadsField))) {
          if (jmethodID getExtPubDir = env->GetStaticMethodID(envClass, "getExternalStoragePublicDirectory", "(Ljava/lang/String;)Ljava/io/File;")) {
            if (jobject fileObj = env->CallStaticObjectMethod(envClass, getExtPubDir, downloadsType)) {
              if (jclass fileClass = env->FindClass("java/io/File")) {
                if (jmethodID getAbsPath = env->GetMethodID(fileClass, "getAbsolutePath", "()Ljava/lang/String;")) {
                  if (jstring pathStr = static_cast<jstring>(env->CallObjectMethod(fileObj, getAbsPath))) {
                    const char *chars = env->GetStringUTFChars(pathStr, nullptr);
                    std::string path = chars ? chars : "";
                    if (chars) env->ReleaseStringUTFChars(pathStr, chars);
                    if (!path.empty()) return path;
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  return getPrimaryStoragePath(env) + "/Download";
}

static std::string resolveTreeUriToPath(JNIEnv *env, std::string_view uri) {
  const std::string fullDecoded = percentDecode(uri);

  // 1. Check if the percent-decoded URI contains a direct /storage/ path
  auto storagePos = fullDecoded.find("/storage/");
  if (storagePos != std::string::npos) {
    std::string path = fullDecoded.substr(storagePos);
    auto queryPos = path.find_first_of("?#");
    if (queryPos != std::string::npos) path.resize(queryPos);
    if (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
  }

  // 2. Check for com.android.externalstorage.documents (tree or document)
  std::size_t authorityPos = uri.find("com.android.externalstorage.documents/tree/");
  std::size_t markerLen = sizeof("com.android.externalstorage.documents/tree/") - 1;
  if (authorityPos == std::string_view::npos) {
    authorityPos = uri.find("com.android.externalstorage.documents/document/");
    markerLen = sizeof("com.android.externalstorage.documents/document/") - 1;
  }

  if (authorityPos != std::string_view::npos) {
    auto encodedId = uri.substr(authorityPos + markerLen);
    std::string decodedId = percentDecode(encodedId);

    auto rawPos = decodedId.find("/storage/");
    if (rawPos != std::string::npos) {
      std::string path = decodedId.substr(rawPos);
      if (path.size() > 1 && path.back() == '/') path.pop_back();
      return path;
    }

    auto colonPos = decodedId.find(':');
    if (colonPos != std::string::npos) {
      const std::string volume = decodedId.substr(0, colonPos);
      std::string relative = decodedId.substr(colonPos + 1);

      std::string base;
      if (volume == "primary") {
        base = getPrimaryStoragePath(env) + "/";
      } else if (!volume.empty()) {
        base = "/storage/" + volume + "/";
      }

      if (!base.empty()) {
        if (!relative.empty()) {
          base += relative;
        } else if (base.back() == '/' && base.size() > 1) {
          base.pop_back();
        }
        return base;
      }
    }
  }

  // 3. Check for com.android.providers.downloads.documents
  if (uri.find("com.android.providers.downloads.documents") != std::string_view::npos) {
    const std::string downloadsDir = getDownloadsDirectoryPath(env);
    std::size_t docPos = fullDecoded.find("/document/");
    if (docPos != std::string::npos) {
      std::string sub = fullDecoded.substr(docPos + 10);
      auto queryPos = sub.find_first_of("?#");
      if (queryPos != std::string::npos) sub.resize(queryPos);

      if (sub.rfind("raw:", 0) == 0) sub = sub.substr(4);
      if (sub.rfind("/storage/", 0) == 0) return sub;

      std::error_code ec;
      if (!sub.empty() && sub != "downloads") {
        std::string candidate = downloadsDir + "/" + sub;
        if (std::filesystem::exists(candidate, ec)) {
          return candidate;
        }
      }
    }
    return downloadsDir;
  }

  // 4. Check for com.android.providers.media.documents
  if (uri.find("com.android.providers.media.documents") != std::string_view::npos) {
    std::size_t docPos = fullDecoded.find("/document/");
    if (docPos != std::string::npos) {
      std::string sub = fullDecoded.substr(docPos + 10);
      auto queryPos = sub.find_first_of("?#");
      if (queryPos != std::string::npos) sub.resize(queryPos);

      if (sub.rfind("raw:", 0) == 0) sub = sub.substr(4);
      if (sub.rfind("/storage/", 0) == 0) return sub;
    }
  }

  return {};
}

// Exposes resolveTreeUriToPath to Kotlin so the UI never needs its own SAF
// tree-URI parsing (e.g. to know what directory prefix to drop from the game
// list when a managed directory is removed). Returns null if unresolvable.
extern "C" jstring _rpcsx_resolveTreeUriToPath(JNIEnv *env,
                                               std::string_view treeUri) {
  const std::string path = resolveTreeUriToPath(env, treeUri);
  if (path.empty()) {
    return nullptr;
  }
  return wrap(env, path);
}

// "Add game folder" flow: resolves the SAF tree URI to a real path and
// delegates to the existing native scanner (collectGameInfo), which already
// detects a single game at the root or recurses into subdirectories looking
// for PARAM.SFO. No copy: the game is registered in place. Returns false if
// the URI cannot be resolved to a real, readable path (e.g. cloud storage) so
// the caller can show an explicit error instead of falling back to a copy.
extern "C" bool _rpcsx_collectGameInfoFromUri(JNIEnv *env,
                                              std::string_view treeUri,
                                              long progressId) {
  const std::string path = resolveTreeUriToPath(env, treeUri);

  if (path.empty()) {
    return false;
  }

  std::error_code ec;
  if (std::filesystem::is_regular_file(path, ec)) {
    auto ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (ext == ".iso") {
      collectIsoInfo(env, progressId, path);
      return true;
    }
    return false;
  }

  if (!std::filesystem::is_directory(path, ec)) {
    return false;
  }

  // Probe readability - this is what actually confirms All-files access, not
  // just that the path syntactically resolved.
  {
    std::error_code probeEc;
    auto it = std::filesystem::directory_iterator(path, probeEc);
    if (probeEc) {
      return false;
    }
  }

  collectGameInfo(env, progressId, {path});
  return true;
}

// "Add ISO directory" flow: resolves the SAF tree URI to a real path and
// scans it (recursively) for loose .iso files, registering each one in
// place - the ISO is played directly from its real path, nothing is copied.
// Returns false if the URI cannot be resolved (same fallback contract as
// _rpcsx_collectGameInfoFromUri).
extern "C" bool _rpcsx_collectIsoInfoFromUri(JNIEnv *env,
                                             std::string_view treeUri,
                                             long progressId) {
  const std::string path = resolveTreeUriToPath(env, treeUri);

  if (path.empty()) {
    return false;
  }

  std::error_code ec;
  if (std::filesystem::is_regular_file(path, ec)) {
    collectIsoInfo(env, progressId, path);
    return true;
  }

  if (!std::filesystem::is_directory(path, ec)) {
    return false;
  }

  {
    std::error_code probeEc;
    auto it = std::filesystem::directory_iterator(path, probeEc);
    if (probeEc) {
      return false;
    }
  }

  collectIsoInfo(env, progressId, path);
  return true;
}

extern "C" bool _rpcsx_installFw(JNIEnv *env, int fd, long progressId) {
  return installPup(env, fs::file::from_native_handle(fd), progressId);
}

extern "C" bool _rpcsx_isInstallableFile(jint fd) {
  auto file = fs::file::from_native_handle(fd);
  AtExit atExit{[&] { file.release_handle(); }};

  auto type = getFileType(file);
  file.seek(0);
  return type != FileType::Unknown &&
         type != FileType::Rap; // FIXME: implement rap preinstallation
}

extern "C" jstring _rpcsx_getDirInstallPath(JNIEnv *env, jint fd) {
  auto file = fs::file::from_native_handle(fd);
  AtExit atExit{[&] { file.release_handle(); }};

  auto psf = psf::load_object(file, "");
  if (auto gameInfo = fetchGameInfo(psf)) {
    return wrap(env, gameInfo->path);
  }

  return nullptr;
}

extern "C" bool _rpcsx_install(JNIEnv *env, int fd, long progressId, std::string_view sourceUri) {
  auto file = fs::file::from_native_handle(fd);
  AtExit atExit{[&] { file.release_handle(); }};

  auto type = getFileType(file);
  file.seek(0);

  switch (type) {
  case FileType::Unknown:
    Progress(env, progressId).failure("Unsupported file type");
    return false;

  case FileType::Pup:
    return installPup(env, std::move(file), progressId);

  case FileType::Pkg:
    return installPkg(env, std::move(file), progressId);

  case FileType::Edat:
    return installEdat(env, std::move(file), progressId);

  case FileType::Iso:
    return installIso(env, std::move(file), progressId, sourceUri);

  case FileType::Rap:
    Progress(env, progressId)
        .failure("RAP file cannot be preinstalled. Use lock button on "
                 "installed game instead");
    return false;
  }

  return true;
}

extern "C" bool _rpcsx_installKey(JNIEnv *env, int fd, long progressId,
                                  std::string_view gamePath) {
  auto file = fs::file::from_native_handle(fd);
  AtExit atExit{[&] { file.release_handle(); }};

  auto type = getFileType(file);
  file.seek(0);

  if (type == FileType::Rap) {
    return installRap(env, std::move(file), progressId, gamePath);
  }

  if (type == FileType::Edat) {
    return installEdat(env, std::move(file), progressId, gamePath);
  }

  Progress(env, progressId).failure("Unsupported key type");
  return false;
}

extern "C" std::string _rpcsx_systemInfo() {
  std::string result;

  fmt::append(result, "%s\n\nLLVM CPU: %s\n\n", utils::get_system_info(),
              fallback_cpu_detection());

  {
    vk::ensure_dynamic_symbols();
    vk::instance device_enum_context;
    if (device_enum_context.create("RPCS3")) {
      device_enum_context.bind();
      const std::vector<vk::physical_device> &gpus =
          device_enum_context.enumerate_devices();

      for (const auto &gpu : gpus) {
        fmt::append(result, "GPU: %s\n\nDriver: v%s\n\n",
                    gpu.get_name(), gpu.get_driver_version());
      }
    }
  }

  return result;
}

static cfg::_base *find_cfg_node(cfg::_base *root, std::string_view path) {
  auto pathList = fmt::split(path, {"@@"});
  std::ranges::reverse(pathList);

  while (!pathList.empty()) {
    auto elem = pathList.back();
    pathList.pop_back();
    if (elem.empty()) {
      continue;
    }

    auto root_node = dynamic_cast<cfg::node *>(root);
    if (root_node == nullptr) {
      return nullptr;
    }

    cfg::_base *child_node = nullptr;

    for (auto node : root_node->get_nodes()) {
      if (node->get_name() == elem) {
        child_node = node;
        break;
      }
    }

    if (child_node == nullptr) {
      return nullptr;
    }

    root = child_node;
  }

  return root;
}

extern "C" void _rpcsx_loginUser(std::string_view userId) {
  Emu.SetUsr(std::string(userId));
}

extern "C" std::string _rpcsx_getUser() { return Emu.GetUsr(); }

// --- Settings bridge --------------------------------------------------------
// The backend owns only the GLOBAL configuration:
//  - config.json is the single source of truth, stored as JSON (JSON is a
//    subset of YAML, so the core parses it with its regular YAML loader).
//  - Writes edit the JSON document and save it through Emulator::SaveSettings.
//    The emulator does NOT need to be stopped: writes are additionally
//    applied to the live g_cfg when the node accepts it (dynamic settings take
//    effect immediately, everything else on next boot) - same semantics as the
//    desktop settings dialog.
//  - Reads serialize the cfg tree to one JSON schema for the Compose UI
//    (per-leaf "type"/"value"/"default", plus "variants" for enums and
//    "min"/"max" for integers), with the saved file values overlaid.
// Per-game configuration lives entirely in the frontend: diff-only
// config_<uuid>_<TITLE_ID>.json files it merges into this schema itself and
// passes at boot via the configPath argument (cfg_mode::custom_selection).

static void json_append_escaped(std::string &out, std::string_view s) {
  out += '"';
  for (char c : s) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        fmt::append(out, "\\u%04x", static_cast<unsigned char>(c));
      } else {
        out += c;
      }
    }
  }
  out += '"';
}

// Child of a YAML map by key, or a Null node when absent/not a map. Null is
// used as the "no overlay" placeholder so callers can chain lookups without
// touching yaml-cpp's throwing undefined nodes.
static YAML::Node yaml_child(const YAML::Node &n, const std::string &key) {
  if (n.IsMap()) {
    YAML::Node c = n[key];
    if (c.IsDefined()) {
      return c;
    }
  }
  return YAML::Node(YAML::NodeType::Null);
}

// Serializes `node` (and children) as the UI schema. `overlay` carries the
// matching subtree of config.json (Null where absent); scalar values found
// there win over the in-memory tree, so the UI always shows what is actually
// saved.
static void cfg_to_json(const cfg::_base *node, const YAML::Node &overlay,
                        std::string &out) {
  const auto effective_value = [&]() -> std::string {
    if (overlay.IsScalar()) {
      return overlay.Scalar();
    }
    return node->to_string();
  };

  switch (node->get_type()) {
  case cfg::type::node: {
    out += '{';
    bool first = true;
    for (const auto &child : static_cast<const cfg::node *>(node)->get_nodes()) {
      if (!first) {
        out += ',';
      }
      first = false;
      json_append_escaped(out, child->get_name());
      out += ':';
      cfg_to_json(child, yaml_child(overlay, child->get_name()), out);
    }
    out += '}';
    break;
  }
  case cfg::type::_bool:
    out += "{\"type\":\"bool\",\"value\":";
    out += effective_value() == "true" ? "true" : "false";
    out += ",\"default\":";
    out += node->def_to_string();
    out += '}';
    break;
  case cfg::type::_enum: {
    out += "{\"type\":\"enum\",\"value\":";
    json_append_escaped(out, effective_value());
    out += ",\"default\":";
    json_append_escaped(out, node->def_to_string());
    out += ",\"variants\":[";
    bool first = true;
    for (const auto &variant : node->to_list()) {
      if (!first) {
        out += ',';
      }
      first = false;
      json_append_escaped(out, variant);
    }
    out += ']';
    out += '}';
    break;
  }
  case cfg::type::_int:
  case cfg::type::uint:
  case cfg::type::uint128:
    out += node->get_type() == cfg::type::_int ? "{\"type\":\"int\""
                                               : "{\"type\":\"uint\"";
    out += ",\"value\":";
    json_append_escaped(out, effective_value());
    out += ",\"default\":";
    json_append_escaped(out, node->def_to_string());
    out += ",\"min\":";
    json_append_escaped(out, node->min_to_string());
    out += ",\"max\":";
    json_append_escaped(out, node->max_to_string());
    out += '}';
    break;
  case cfg::type::string:
    out += "{\"type\":\"string\",\"value\":";
    json_append_escaped(out, effective_value());
    out += ",\"default\":";
    json_append_escaped(out, node->def_to_string());
    out += '}';
    break;
  case cfg::type::set: {
    out += "{\"type\":\"set\",\"value\":[";
    bool first = true;
    for (const auto &item : node->to_list()) {
      if (!first) {
        out += ',';
      }
      first = false;
      json_append_escaped(out, item);
    }
    out += "]}";
    break;
  }
  default:
    // map/node_map/log/device: not rendered by the settings UI
    out += "{\"type\":\"unknown\"}";
    break;
  }
}

// The UI sends a single JSON scalar: `true`/`false`, a bare number, or a
// quoted string ("Vulkan"). Convert it to the plain string form vanilla
// cfg::from_string expects.
static bool json_scalar_to_string(std::string_view json, std::string &out) {
  while (!json.empty() && (json.front() == ' ' || json.front() == '\t')) {
    json.remove_prefix(1);
  }
  while (!json.empty() && (json.back() == ' ' || json.back() == '\t')) {
    json.remove_suffix(1);
  }

  if (json.empty()) {
    return false;
  }

  if (json.front() == '"') {
    if (json.size() < 2 || json.back() != '"') {
      return false;
    }

    json.remove_prefix(1);
    json.remove_suffix(1);

    out.clear();
    for (std::size_t i = 0; i < json.size(); ++i) {
      if (json[i] != '\\') {
        out += json[i];
        continue;
      }

      if (++i >= json.size()) {
        return false;
      }

      switch (json[i]) {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      default: return false; // \uXXXX etc. never appear in settings values
      }
    }
    return true;
  }

  // bare scalar: true/false/number
  out = std::string(json);
  return true;
}

// The file backing the global scope: config.json.
static std::string settings_file_path() {
  return fs::get_config_dir(true) + "config.json";
}

static YAML::Node settings_file_load() {
  std::string text;
  if (fs::file f{settings_file_path()}) {
    text = f.to_string();
  }

  auto [root, error] = yaml_load(text);
  if (!error.empty() || !root.IsMap()) {
    return YAML::Node(YAML::NodeType::Map);
  }
  return root;
}

// Serializes a YAML tree as pretty-printed JSON. Every scalar is emitted as a
// JSON string; quoted scalars parse back identically through yaml_load, so
// the round-trip is lossless for the cfg tree (all values are strings there).
static void yaml_to_json(const YAML::Node &n, std::string &out, int indent) {
  const auto newline_indent = [&](int level) {
    out += '\n';
    out.append(level * 2, ' ');
  };

  switch (n.Type()) {
  case YAML::NodeType::Map: {
    if (n.size() == 0) {
      out += "{}";
      break;
    }
    out += '{';
    bool first = true;
    for (const auto &kv : n) {
      if (!first) {
        out += ',';
      }
      first = false;
      newline_indent(indent + 1);
      json_append_escaped(out, kv.first.Scalar());
      out += ": ";
      yaml_to_json(kv.second, out, indent + 1);
    }
    newline_indent(indent);
    out += '}';
    break;
  }
  case YAML::NodeType::Sequence: {
    out += '[';
    bool first = true;
    for (const auto &item : n) {
      if (!first) {
        out += ", ";
      }
      first = false;
      yaml_to_json(item, out, indent);
    }
    out += ']';
    break;
  }
  case YAML::NodeType::Scalar:
    json_append_escaped(out, n.Scalar());
    break;
  default:
    // Null/undefined (e.g. an empty "Log:" section): an empty object keeps
    // the file valid JSON and decodes back to an empty map.
    out += "{}";
    break;
  }
}

static void settings_file_save(const YAML::Node &root) {
  std::string json;
  yaml_to_json(root, json, 0);
  json += '\n';
  Emulator::SaveSettings(json, {});
}

// Persists the live g_cfg to the global config.json as JSON. Also normalizes
// files written as YAML by older builds (or by the core's first-boot default
// dump) the next time it runs.
static void save_global_config() {
  auto [root, error] = yaml_load(g_cfg.to_string());
  if (!error.empty()) {
    rpcsx_android.error("save_global_config: failed to serialize g_cfg: %s",
                        error);
    return;
  }
  settings_file_save(root);
}

// Schema for the settings UI, global scope only. titleId is ignored (kept
// for JNI compatibility); per-game overrides live in the frontend.
extern "C" std::string _rpcsx_configGet(std::string_view titleId,
                                          std::string_view path) {
  auto node = find_cfg_node(&g_cfg, path);

  if (node == nullptr) {
    return {};
  }

  YAML::Node overlay = settings_file_load();

  for (const auto &seg : fmt::split(path, {"@@"})) {
    overlay = yaml_child(overlay, seg);
  }

  std::string result;
  cfg_to_json(node, overlay, result);
  return result;
}

extern "C" bool _rpcsx_configSet(std::string_view titleId,
                                   std::string_view path,
                                   std::string_view valueString) {
  auto node = find_cfg_node(&g_cfg, path);

  if (node == nullptr) {
    rpcsx_android.error("settingsSet: node %s not found", path);
    return false;
  }

  std::string value;
  if (!json_scalar_to_string(valueString, value)) {
    rpcsx_android.error("settingsSet: node %s passed with invalid json '%s'",
                        path, valueString);
    return false;
  }

  auto segments = fmt::split(path, {"@@"});
  if (segments.empty()) {
    return false;
  }

  YAML::Node root = settings_file_load();
  YAML::Node cur = root;
  for (std::size_t i = 0; i + 1 < segments.size(); i++) {
    YAML::Node next = cur[segments[i]];
    if (!next.IsMap()) {
      cur[segments[i]] = YAML::Node(YAML::NodeType::Map);
      next.reset(cur[segments[i]]);
    }
    cur.reset(next);
  }
  cur[segments.back()] = value;

  settings_file_save(root);

  // Best-effort live apply: dynamic settings take effect immediately, the
  // rest are already saved and apply on next boot. Skipped while a per-game
  // config is active (GetUsedConfig() non-empty): the running g_cfg carries
  // that game's overrides, and pushing a global value over them would undo
  // the per-game setting mid-session - same rule the desktop dialog applies.
  if (Emu.GetUsedConfig().empty()) {
    node->from_string(value, !Emu.IsStopped());
  }

  return true;
}

extern "C" bool _rpcsx_configRemove(std::string_view titleId,
                                      std::string_view path) {
  auto segments = fmt::split(path, {"@@"});
  if (segments.empty()) {
    return false; // never wipe the whole global config
  }

  YAML::Node root = settings_file_load();

  // Walk to the leaf's parent, remembering the chain to prune empty maps.
  std::vector<std::pair<YAML::Node, std::string>> chain;
  YAML::Node cur = root;
  for (std::size_t i = 0; i + 1 < segments.size(); i++) {
    chain.emplace_back(cur, segments[i]);
    YAML::Node next = cur[segments[i]];
    if (!next.IsMap()) {
      return true; // nothing to remove
    }
    cur.reset(next);
  }
  cur.remove(segments.back());

  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    YAML::Node child = it->first[it->second];
    if (child.IsMap() && child.size() == 0) {
      it->first.remove(it->second);
    }
  }

  settings_file_save(root);
  return true;
}

// Live-applies one per-game override to the currently running game WITHOUT
// touching config.json - the frontend owns the per-game file and reapplies
// it at every boot; this only pushes the value into the live g_cfg. An empty
// valueString reverts the node to its effective global value (used when an
// override is removed). Only dynamic settings are pushed while a game runs -
// the same gate cfg::decode applies - so non-dynamic edits are a successful
// no-op that simply waits for the next boot.
extern "C" bool _rpcsx_configLiveApply(std::string_view path,
                                       std::string_view valueString) {
  auto node = find_cfg_node(&g_cfg, path);
  if (node == nullptr) {
    rpcsx_android.error("configLiveApply: node %s not found", path);
    return false;
  }

  if (Emu.IsStopped() || !node->get_is_dynamic()) {
    return true;
  }

  std::string value;
  if (valueString.empty()) {
    YAML::Node overlay = settings_file_load();
    for (const auto &seg : fmt::split(path, {"@@"})) {
      overlay = yaml_child(overlay, seg);
    }
    value = overlay.IsScalar() ? overlay.Scalar() : node->def_to_string();
  } else if (!json_scalar_to_string(valueString, value)) {
    rpcsx_android.error("configLiveApply: node %s passed with invalid json '%s'",
                        path, valueString);
    return false;
  }

  return node->from_string(value, true);
}

// --- Pad tuning configuration ------------------------------------------
// Per-player-slot bridge onto g_cfg_input.player[i]->config (a cfg_pad) -
// the same per-player pad config desktop RPCS3's Qt pad dialog reads/writes
// (rpcs3qt/pad_settings_dialog.cpp). Global scope only, no per-game override
// (cfg_pad isn't a per-game concept upstream either). setVirtualPadData
// reads these fields live off this same tree on every input event, so a
// write here takes effect on the very next event - no pad::reset needed,
// unlike a handler/device change.
static cfg_pad *pad_tuning_cfg(int playerIndex) {
  if (playerIndex < 0 ||
      playerIndex >= static_cast<int>(g_cfg_input.player.size())) {
    return nullptr;
  }
  return &g_cfg_input.player[playerIndex]->config;
}

// Schema for one leaf/subtree of a player's pad config, same shape
// _rpcsx_configGet produces (type/value/default/min/max/variants). No JSON
// overlay file exists for pad config (unlike config.json) - the live tree
// already is what gets persisted, so effective_value() reads straight off
// it (null overlay).
extern "C" std::string _rpcsx_padConfigGet(int playerIndex,
                                            std::string_view path) {
  auto *cfg = pad_tuning_cfg(playerIndex);
  if (cfg == nullptr) {
    return {};
  }

  auto node = find_cfg_node(cfg, path);
  if (node == nullptr) {
    return {};
  }

  std::string result;
  cfg_to_json(node, YAML::Node(YAML::NodeType::Null), result);
  return result;
}

extern "C" bool _rpcsx_padConfigSet(int playerIndex, std::string_view path,
                                     std::string_view valueString) {
  auto *cfg = pad_tuning_cfg(playerIndex);
  if (cfg == nullptr) {
    return false;
  }

  auto node = find_cfg_node(cfg, path);
  if (node == nullptr) {
    rpcsx_android.error("padConfigSet: node %s not found for player %d", path,
                        playerIndex);
    return false;
  }

  std::string value;
  if (!json_scalar_to_string(valueString, value)) {
    rpcsx_android.error("padConfigSet: node %s passed with invalid json '%s'",
                        path, valueString);
    return false;
  }

  if (!node->from_string(value, !Emu.IsStopped())) {
    return false;
  }

  g_cfg_input.save("", g_cfg_input_configs.default_config);
  return true;
}

extern "C" bool _rpcsx_padConfigResetToDefault(int playerIndex,
                                                std::string_view path) {
  auto *cfg = pad_tuning_cfg(playerIndex);
  if (cfg == nullptr) {
    return false;
  }

  auto node = find_cfg_node(cfg, path);
  if (node == nullptr) {
    return false;
  }

  node->from_default();
  g_cfg_input.save("", g_cfg_input_configs.default_config);
  return true;
}

// --- Per-game custom configuration ------------------------------------------
// Mirrors desktop RPCS3's per-title configs (config/custom_configs/
// config_<serial>.yml, cfg_mode::custom at boot - see rpcs3::utils::
// get_custom_config_path). The serial is the game's TITLE_ID, not a
// per-registration id: this is a storage-only bridge, the same split
// GameConfig.kt used to keep client-side (Kotlin merges these raw overrides
// onto the global schema from _rpcsx_configGet to render "value"/
// "overridden") - only the storage backend moved here, so a custom config now
// keys purely by title id and is shared by every source of the same game
// (an installed copy, an ISO, a loose folder), matching desktop.

static void ensure_custom_config_dir() {
  fs::create_path(rpcs3::utils::get_custom_config_dir());
}

// Renders a YAML tree as JSON, scalars as JSON strings (customConfigSet/
// Import always persist the plain-string form via json_scalar_to_string, so
// every leaf read back is a string here too - the settings UI already reads
// every value through optString, so this needs no further typing).
static void yaml_to_json(const YAML::Node &node, std::string &out) {
  if (node.IsMap()) {
    out += '{';
    bool first = true;
    for (const auto &kv : node) {
      if (!kv.first.IsScalar()) {
        continue;
      }
      if (!first) {
        out += ',';
      }
      first = false;
      json_append_escaped(out, kv.first.Scalar());
      out += ':';
      yaml_to_json(kv.second, out);
    }
    out += '}';
  } else if (node.IsScalar()) {
    json_append_escaped(out, node.Scalar());
  } else {
    out += "null";
  }
}

// Recursive leaf walker for _rpcsx_customConfigImportYaml: validates each
// scalar leaf of `node` against `scratchRoot` (the live global schema) and
// writes accepted ones into `outRoot`, same acceptance rule customConfigSet
// uses. Not a local lambda so the whole file keeps its one style (plain
// static helpers) instead of mixing in std::function.
static void import_yaml_leaf(const YAML::Node &node, const std::string &prefix,
                             cfg::_base *scratchRoot, YAML::Node &outRoot,
                             bool &wroteAny) {
  if (!node.IsMap()) {
    return;
  }

  for (const auto &kv : node) {
    if (!kv.first.IsScalar()) {
      continue;
    }
    const std::string key = kv.first.Scalar();
    const std::string path = prefix.empty() ? key : prefix + "@@" + key;

    if (kv.second.IsMap()) {
      import_yaml_leaf(kv.second, path, scratchRoot, outRoot, wroteAny);
      continue;
    }
    if (!kv.second.IsScalar()) {
      continue;
    }

    auto schema = find_cfg_node(scratchRoot, path);
    if (schema == nullptr) {
      rpcsx_android.error("customConfigImportYaml: unknown key '%s' skipped",
                          path);
      continue;
    }

    const std::string scalar = kv.second.Scalar();
    if (!schema->from_string(scalar, false)) {
      rpcsx_android.error(
          "customConfigImportYaml: value '%s' rejected for key '%s'", scalar,
          path);
      continue;
    }

    auto pathList = fmt::split(path, {"@@"});
    YAML::Node cur;
    cur.reset(outRoot);
    for (std::size_t i = 0; i < pathList.size(); i++) {
      if (i + 1 == pathList.size()) {
        cur[pathList[i]] = scalar;
        break;
      }
      YAML::Node next = cur[pathList[i]];
      if (!next.IsMap()) {
        cur[pathList[i]] = YAML::Node(YAML::NodeType::Map);
        next.reset(cur[pathList[i]]);
      }
      cur.reset(next);
    }
    wroteAny = true;
  }
}

// True if a custom config file exists for this title.
extern "C" bool _rpcsx_customConfigExists(std::string_view serial) {
  if (serial.empty()) {
    return false;
  }
  return fs::is_file(rpcs3::utils::get_custom_config_path(std::string(serial)));
}

// Deletes a title's custom config; subsequent boots fall back to the global
// config entirely.
extern "C" bool _rpcsx_customConfigDelete(std::string_view serial) {
  if (serial.empty()) {
    return false;
  }
  return fs::remove_file(
      rpcs3::utils::get_custom_config_path(std::string(serial)));
}

// Raw content of the custom config file as JSON (nested map of overridden
// leaf paths -> scalar strings), or "{}" if none exists yet. Kotlin merges
// this onto the global schema (_rpcsx_configGet) - same split GameConfig.kt
// used to do locally, just backed by this file instead of a uuid-keyed one.
extern "C" std::string
_rpcsx_customConfigGetOverrides(std::string_view serial) {
  if (serial.empty()) {
    return "{}";
  }

  fs::file f{rpcs3::utils::get_custom_config_path(std::string(serial))};
  if (!f) {
    return "{}";
  }

  YAML::Node root;
  try {
    root = YAML::Load(f.to_string());
  } catch (...) {
    return "{}";
  }

  if (!root.IsMap()) {
    return "{}";
  }

  std::string out;
  yaml_to_json(root, out);
  return out;
}

// Validates and persists one leaf into the title's custom config file
// (created on first edit). Mirrors _rpcsx_configSet's YAML tree-walk, but
// against config/custom_configs/config_<serial>.yml instead of config.json,
// and never live-applies (the Kotlin side already calls the existing
// _rpcsx_configLiveApply for the currently booted game, same as before).
extern "C" bool _rpcsx_customConfigSet(std::string_view serial,
                                       std::string_view path,
                                       std::string_view valueString) {
  if (serial.empty()) {
    return false;
  }

  // Validate against a scratch tree seeded from the live global config, so
  // enum/range acceptance matches what boot will actually apply. Never
  // touches g_cfg itself.
  cfg_root scratch;
  scratch.from_string(g_cfg.to_string());
  auto node = find_cfg_node(&scratch, path);
  if (node == nullptr) {
    rpcsx_android.error("customConfigSet: node %s not found", path);
    return false;
  }

  std::string value;
  if (!json_scalar_to_string(valueString, value)) {
    rpcsx_android.error("customConfigSet: node %s passed with invalid json '%s'",
                        path, valueString);
    return false;
  }

  if (!node->from_string(value, false)) {
    rpcsx_android.error("customConfigSet: node %s not accepts value '%s'", path,
                        value);
    return false;
  }

  auto segments = fmt::split(path, {"@@"});
  if (segments.empty()) {
    return false;
  }

  YAML::Node root;
  if (fs::file f{rpcs3::utils::get_custom_config_path(std::string(serial))}) {
    try {
      root = YAML::Load(f.to_string());
    } catch (...) {
      rpcsx_android.error(
          "customConfigSet: existing custom config unreadable, recreating");
    }
  }
  if (!root.IsMap()) {
    root = YAML::Node(YAML::NodeType::Map);
  }

  YAML::Node cur = root;
  for (std::size_t i = 0; i + 1 < segments.size(); i++) {
    YAML::Node next = cur[segments[i]];
    if (!next.IsMap()) {
      cur[segments[i]] = YAML::Node(YAML::NodeType::Map);
      next.reset(cur[segments[i]]);
    }
    cur.reset(next);
  }
  cur[segments.back()] = value;

  ensure_custom_config_dir();
  Emulator::SaveSettings(YAML::Dump(root) + "\n", std::string(serial));
  return true;
}

// Removes one override (prunes empty parent maps). Mirrors
// _rpcsx_configRemove, targeting the per-title custom file instead.
extern "C" bool _rpcsx_customConfigRemove(std::string_view serial,
                                          std::string_view path) {
  if (serial.empty()) {
    return false;
  }
  auto segments = fmt::split(path, {"@@"});
  if (segments.empty()) {
    return false;
  }

  fs::file f{rpcs3::utils::get_custom_config_path(std::string(serial))};
  if (!f) {
    return true; // nothing to remove
  }

  YAML::Node root;
  try {
    root = YAML::Load(f.to_string());
  } catch (...) {
    return true;
  }
  if (!root.IsMap()) {
    return true;
  }

  std::vector<std::pair<YAML::Node, std::string>> chain;
  YAML::Node cur = root;
  for (std::size_t i = 0; i + 1 < segments.size(); i++) {
    chain.emplace_back(cur, segments[i]);
    YAML::Node next = cur[segments[i]];
    if (!next.IsMap()) {
      return true; // nothing to remove
    }
    cur.reset(next);
  }
  cur.remove(segments.back());

  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    YAML::Node child = it->first[it->second];
    if (child.IsMap() && child.size() == 0) {
      it->first.remove(it->second);
    }
  }

  ensure_custom_config_dir();
  Emulator::SaveSettings(YAML::Dump(root) + "\n", std::string(serial));
  return true;
}

// Applies a community/recommended config (sparse YAML, one key per changed
// setting - see PerGameConfigRepository.fetchCommunityConfig) as this title's
// custom config. Each leaf is validated against the live schema before being
// written, same rule as customConfigSet; unmentioned settings keep
// inheriting the user's globals at boot. Merges into any existing custom
// file so re-applying doesn't drop prior manual edits.
extern "C" bool _rpcsx_customConfigImportYaml(std::string_view serial,
                                              std::string_view yaml) {
  if (serial.empty()) {
    return false;
  }

  YAML::Node incoming;
  try {
    incoming = YAML::Load(std::string(yaml));
  } catch (...) {
    rpcsx_android.error("customConfigImportYaml: unparseable YAML for %s",
                        serial);
    return false;
  }
  if (!incoming.IsMap()) {
    rpcsx_android.error(
        "customConfigImportYaml: top-level YAML is not a map for %s", serial);
    return false;
  }

  YAML::Node root;
  if (fs::file f{rpcs3::utils::get_custom_config_path(std::string(serial))}) {
    try {
      root = YAML::Load(f.to_string());
    } catch (...) {
      rpcsx_android.error("customConfigImportYaml: existing custom config "
                          "unreadable, recreating");
    }
  }
  if (!root.IsMap()) {
    root = YAML::Node(YAML::NodeType::Map);
  }

  cfg_root scratch;
  scratch.from_string(g_cfg.to_string());

  bool wroteAny = false;
  import_yaml_leaf(incoming, "", &scratch, root, wroteAny);

  if (!wroteAny) {
    rpcsx_android.error("customConfigImportYaml: no valid keys in preset for %s",
                        serial);
    return false;
  }

  ensure_custom_config_dir();
  Emulator::SaveSettings(YAML::Dump(root) + "\n", std::string(serial));
  return _rpcsx_customConfigExists(serial);
}

extern "C" std::string _rpcsx_getVersion() {
  return rpcs3::get_version().to_string();
}

extern "C" void *_rpcsx_setCustomDriver(void *driverHandle) {
  // driverHandle comes from adrenotools_open_libvulkan() on the UI side;
  // nullptr switches back to the system libvulkan. Returns the previous
  // handle so the UI can dlclose it.
  rpcsx_android.notice("setCustomDriver(%p)", driverHandle);
  return vk::load_dynamic_symbols(driverHandle);
}

// Dummy implementation to satisfy RtMidi linkage on Android
extern "C" jint JNI_GetCreatedJavaVMs(JavaVM **vmBuf, jsize bufLen, jsize *nVMs) {
  if (nVMs) *nVMs = 0;
  return 0; // JNI_OK
}

// Dummy implementation to satisfy curl linkage on Android
extern "C" int wolfSSL_CTX_set1_groups_list(void* ctx, const char* list) {
  return 1; // 1 = success
}

#pragma GCC diagnostic pop
