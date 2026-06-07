#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr guint kAgentPort = 8765;
constexpr int kProtocolVersion = 16;
constexpr const char* kAppVersion = "0.2.0";

struct AppState {
  GSocketService* service = nullptr;
  GSubprocess* ffmpeg = nullptr;
  GSubprocess* capture = nullptr;
  GSubprocess* link = nullptr;
  std::string command_display;
  std::string last_error;
  std::string ffmpeg_error_path;
  std::string capture_error_path;
  std::string link_error_path;
  int exit_code = 0;
  int link_attempts = 0;
  int capture_node_id = -1;
  int link_output_port = -1;
  int link_input_port = -1;
  guint link_retry_source = 0;
  bool link_established = false;
};

AppState state;

std::string data_path(const char* filename) {
  const std::string directory =
      std::string(g_get_user_data_dir()) + "/DeckScreenShare";
  g_mkdir_with_parents(directory.c_str(), 0755);
  return directory + "/" + filename;
}

void write_log(const std::string& message) {
  const std::string path = data_path("agent.log");
  GDateTime* now = g_date_time_new_now_local();
  gchar* timestamp = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
  const std::string line =
      std::string(timestamp ? timestamp : "") + " " + message + "\n";
  gchar* existing_data = nullptr;
  gsize existing_size = 0;
  g_file_get_contents(path.c_str(), &existing_data, &existing_size, nullptr);
  std::string contents = existing_data ? existing_data : "";
  g_free(existing_data);
  contents += line;
  constexpr std::size_t kMaxLogLength = 100000;
  if (contents.size() > kMaxLogLength) {
    contents.erase(0, contents.size() - kMaxLogLength);
  }
  g_file_set_contents(path.c_str(), contents.c_str(), contents.size(), nullptr);
  g_free(timestamp);
  g_date_time_unref(now);
}

std::string json_string(JsonObject* object, const char* key,
                        const char* fallback = "") {
  if (!json_object_has_member(object, key)) {
    return fallback;
  }
  return json_object_get_string_member(object, key);
}

int json_int(JsonObject* object, const char* key, int fallback, int minimum,
             int maximum) {
  const int value = json_object_has_member(object, key)
                        ? static_cast<int>(json_object_get_int_member(object, key))
                        : fallback;
  if (value < minimum || value > maximum) {
    throw std::runtime_error(std::string(key) + " is outside the allowed range");
  }
  return value;
}

std::pair<int, int> video_size(JsonObject* config) {
  std::istringstream value(json_string(config, "size", "1280x800"));
  int width = 0;
  int height = 0;
  char separator = '\0';
  if (!(value >> width >> separator >> height) || separator != 'x' ||
      width < 16 || width > 7680 || height < 16 || height > 4320) {
    throw std::runtime_error("size must use WIDTHxHEIGHT format");
  }
  return {width, height};
}

void append(std::vector<std::string>& args,
            std::initializer_list<std::string> values) {
  args.insert(args.end(), values.begin(), values.end());
}

std::vector<std::string> device_paths(const char* prefix) {
  std::vector<std::string> paths;
  GError* error = nullptr;
  GDir* directory = g_dir_open("/dev/dri", 0, &error);
  if (directory == nullptr) {
    g_clear_error(&error);
    return paths;
  }
  while (const gchar* name = g_dir_read_name(directory)) {
    if (g_str_has_prefix(name, prefix)) {
      paths.push_back(std::string("/dev/dri/") + name);
    }
  }
  g_dir_close(directory);
  std::sort(paths.begin(), paths.end());
  return paths;
}

std::string available_device(JsonObject* config, const char* key,
                             const char* prefix) {
  const std::string requested = json_string(config, key);
  if (!requested.empty() && g_file_test(requested.c_str(), G_FILE_TEST_EXISTS)) {
    return requested;
  }
  const auto paths = device_paths(prefix);
  if (!paths.empty()) return paths.front();
  throw std::runtime_error(std::string("No /dev/dri/") + prefix +
                           "* device is available inside the Flatpak");
}

bool pipewire_manager_available() {
  const gchar* runtime = g_get_user_runtime_dir();
  if (runtime == nullptr) return false;
  return g_file_test((std::string(runtime) + "/pipewire-0-manager").c_str(),
                     G_FILE_TEST_EXISTS);
}

std::string file_contents(const std::string& path) {
  if (path.empty()) return "";
  gchar* data = nullptr;
  gsize size = 0;
  g_file_get_contents(path.c_str(), &data, &size, nullptr);
  std::string contents = data ? std::string(data, size) : "";
  g_free(data);
  return contents;
}

std::string pipewire_command_output(const std::vector<std::string>& args) {
  std::vector<const gchar*> argv;
  for (const auto& arg : args) argv.push_back(arg.c_str());
  argv.push_back(nullptr);
  GSubprocessLauncher* launcher = g_subprocess_launcher_new(
      static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                    G_SUBPROCESS_FLAGS_STDERR_PIPE));
  g_subprocess_launcher_setenv(launcher, "PIPEWIRE_REMOTE",
                               "pipewire-0-manager", TRUE);
  GError* error = nullptr;
  GSubprocess* process =
      g_subprocess_launcher_spawnv(launcher, argv.data(), &error);
  g_object_unref(launcher);
  if (process == nullptr) {
    const std::string message = error ? error->message : "Command failed";
    g_clear_error(&error);
    return message;
  }
  gchar* stdout_data = nullptr;
  gchar* stderr_data = nullptr;
  g_subprocess_communicate_utf8(process, nullptr, nullptr, &stdout_data,
                                &stderr_data, &error);
  std::string output = stdout_data ? stdout_data : "";
  if (output.empty() && stderr_data != nullptr) output = stderr_data;
  if (error != nullptr) output = error->message;
  g_free(stdout_data);
  g_free(stderr_data);
  g_clear_error(&error);
  g_object_unref(process);
  return output;
}

int gamescope_node_id() {
  const std::string nodes =
      pipewire_command_output({"pw-cli", "list-objects", "Node"});
  std::istringstream lines(nodes);
  std::string line;
  int current_id = -1;
  int gamescope_id = -1;
  while (std::getline(lines, line)) {
    std::istringstream values(line);
    std::string key;
    if (values >> key && key == "id") {
      int id = -1;
      if (values >> id) current_id = id;
    }
    if (current_id >= 0 &&
        line.find("node.name = \"gamescope\"") != std::string::npos) {
      gamescope_id = std::max(gamescope_id, current_id);
    }
  }
  if (gamescope_id >= 0) return gamescope_id;
  throw std::runtime_error("Gamescope PipeWire video node is unavailable");
}

int pipewire_port_id(bool output, const std::string& node_name) {
  const std::string ports =
      pipewire_command_output({"pw-link", output ? "-oI" : "-iI"});
  std::istringstream lines(ports);
  std::string line;
  int port_id = -1;
  while (std::getline(lines, line)) {
    if (line.find(node_name + ":") == std::string::npos) continue;
    std::istringstream values(line);
    int id = -1;
    if (values >> id) port_id = std::max(port_id, id);
  }
  return port_id;
}

std::vector<std::string> build_ffmpeg_command(JsonObject* config) {
  const std::string codec = json_string(config, "codec", "h264");
  const std::string backend = json_string(config, "backend", "software");
  const std::string capture_mode = json_string(config, "capture_mode", "pipewire");
  const std::string receiver_host = json_string(config, "receiver_host");
  const int srt_port = json_int(config, "srt_port", 9000, 1, 65532);
  const int fps = json_int(config, "fps", 60, 1, 240);
  const std::string timing_filter =
      "settb=AVTB,setpts=N/(" + std::to_string(fps) + "*TB)";
  const int video_bitrate =
      json_int(config, "video_bitrate_kbps", 8000, 250, 100000);
  const int audio_bitrate =
      json_int(config, "audio_bitrate_kbps", 160, 32, 1024);
  json_int(config, "latency_ms", 120, 20, 5000);

  if (receiver_host.empty() ||
      receiver_host.find_first_of("/?& \t\r\n") != std::string::npos) {
    throw std::runtime_error("receiver_host is invalid");
  }

  static const std::unordered_map<std::string,
                                  std::unordered_map<std::string, std::string>>
      encoders = {
          {"h264", {{"software", "libx264"}, {"vaapi", "h264_vaapi"}}},
          {"h265", {{"software", "libx265"}, {"vaapi", "hevc_vaapi"}}},
          {"av1", {{"software", "libsvtav1"}, {"vaapi", "av1_vaapi"}}},
      };
  if (!encoders.count(codec) || !encoders.at(codec).count(backend)) {
    throw std::runtime_error("Unsupported codec or encoder backend");
  }

  std::vector<std::string> args = {
      "ffmpeg", "-hide_banner", "-loglevel", "warning", "-y"};
  if (capture_mode == "pipewire") {
    append(args, {"-thread_queue_size", "1024", "-r", std::to_string(fps),
                  "-f", "yuv4mpegpipe", "-i", "pipe:0"});
  } else if (capture_mode == "kmsgrab") {
    append(args, {"-f", "kmsgrab", "-device",
                  available_device(config, "drm_device", "card"),
                  "-framerate", std::to_string(fps), "-i", "-"});
  } else if (capture_mode == "x11grab") {
    append(args, {"-f", "x11grab", "-draw_mouse", "1", "-framerate",
                  std::to_string(fps), "-video_size",
                  json_string(config, "size", "1280x800"), "-i",
                  json_string(config, "display", ":0.0")});
  } else {
    throw std::runtime_error(
        "capture_mode must be pipewire, kmsgrab, or x11grab");
  }

  append(args, {"-thread_queue_size", "1024", "-f", "pulse", "-i",
                json_string(config, "audio_source", "@DEFAULT_MONITOR@")});

  const std::string encoder = encoders.at(codec).at(backend);
  if (backend == "vaapi") {
    append(args, {"-vaapi_device",
                  available_device(config, "vaapi_device", "renderD")});
    const std::string filter =
        capture_mode == "kmsgrab"
            ? "hwmap=derive_device=vaapi,scale_vaapi=format=nv12"
            : (capture_mode == "pipewire" ? timing_filter + "," : "") +
                  "format=nv12,hwupload";
    append(args, {"-vf", filter});
  } else {
    if (capture_mode == "kmsgrab") {
      append(args, {"-vf", "hwdownload,format=bgr0"});
    } else if (capture_mode == "pipewire") {
      append(args, {"-vf", timing_filter});
    }
    if (encoder == "libx264" || encoder == "libx265") {
      append(args, {"-preset", "veryfast", "-tune", "zerolatency"});
    } else if (encoder == "libsvtav1") {
      append(args, {"-preset", "10"});
    }
  }

  const std::string video_rate = std::to_string(video_bitrate) + "k";
  const std::string transport_format = codec == "av1" ? "nut" : "mpegts";
  const std::string receiver_url =
      "[select=v:f=" + transport_format + ":onfail=ignore]udp://" + receiver_host + ":" +
      std::to_string(srt_port) +
      "?pkt_size=1316|[select=v:f=" + transport_format + ":onfail=ignore]udp://" + receiver_host + ":" +
      std::to_string(srt_port + 1) +
      "?pkt_size=1316|[select=a:f=" + transport_format + ":onfail=ignore]udp://" + receiver_host + ":" +
      std::to_string(srt_port + 2) +
      "?pkt_size=1316|[select=a:f=" + transport_format + ":onfail=ignore]udp://" + receiver_host +
      ":" + std::to_string(srt_port + 3) + "?pkt_size=1316";
  append(args, {"-map", "0:v:0", "-map", "1:a:0", "-af",
                "asetpts=N/SR/TB", "-r", std::to_string(fps), "-fps_mode",
                "cfr", "-c:v", encoder, "-b:v",
                video_rate, "-maxrate", video_rate, "-bufsize",
                std::to_string(video_bitrate * 2) + "k", "-g",
                std::to_string(fps * 2)});
  if (codec == "h264" || codec == "h265") {
    append(args, {"-bsf:v", "dump_extra=freq=keyframe"});
  }
  append(args, {"-c:a", "libopus", "-b:a",
                std::to_string(audio_bitrate) + "k", "-ar", "48000",
                "-muxdelay", "0", "-muxpreload", "0", "-f", "tee",
                receiver_url});
  return args;
}

std::vector<std::string> build_pipewire_command(JsonObject* config) {
  const int fps = json_int(config, "fps", 60, 1, 240);
  const auto [width, height] = video_size(config);
  const long long data_rate =
      static_cast<long long>(width) * height * 3 * fps / 2;
  if (data_rate > G_MAXINT) {
    throw std::runtime_error("Requested size and FPS are too large");
  }
  const std::string pipeline =
      json_string(config, "pipewire_pipeline", "vaapi");
  state.capture_node_id = gamescope_node_id();
  std::vector<std::string> args = {
      "gst-launch-1.0", "-q", "pipewiresrc", "autoconnect=false",
      "do-timestamp=true"};
  if (pipeline == "vaapi") {
    append(args, {"!", "vapostproc"});
  } else if (pipeline == "vulkan") {
    append(args, {"!", "vulkanupload", "!", "vulkancolorconvert", "!",
                  "vulkandownload", "!", "video/x-raw,format=RGBA", "!",
                  "videoconvert"});
  } else if (pipeline == "opengl") {
    append(args, {"!", "glupload", "!", "glcolorconvert", "!", "gldownload",
                  "!", "video/x-raw,format=RGBA", "!", "videoconvert"});
  } else if (pipeline == "cpu") {
    args.push_back("always-copy=true");
    append(args, {"!", "queue", "!", "videoconvert"});
  } else {
    throw std::runtime_error(
        "pipewire_pipeline must be vaapi, vulkan, opengl, or cpu");
  }
  append(args, {"!", "video/x-raw,format=I420,width=" +
                           std::to_string(width) + ",height=" +
                           std::to_string(height),
                "!", "identity", "datarate=" + std::to_string(data_rate), "!",
                "clocksync", "sync-to-first=true", "!", "y4menc", "!",
                "fdsink", "fd=1"});
  return args;
}

gboolean start_pipewire_link(gpointer);

void link_exited(GObject* source, GAsyncResult* result, gpointer) {
  auto* process = G_SUBPROCESS(source);
  g_subprocess_wait_finish(process, result, nullptr);
  if (state.link != process) return;
  const int exit_code = g_subprocess_get_if_exited(process)
                            ? g_subprocess_get_exit_status(process)
                            : -1;
  const std::string error = file_contents(state.link_error_path);
  write_log("PipeWire link exited with code " + std::to_string(exit_code) +
            (error.empty() ? "" : ": " + error));
  const bool linked = state.capture != nullptr && state.ffmpeg != nullptr;
  g_clear_object(&state.link);
  if (exit_code == 0 || error.find("File exists") != std::string::npos) {
    state.link_established = true;
    g_file_set_contents(state.link_error_path.c_str(), "", 0, nullptr);
  } else if (linked && state.link_attempts < 30 &&
             state.link_retry_source == 0) {
    state.link_retry_source = g_timeout_add(200, start_pipewire_link, nullptr);
  }
}

gboolean start_pipewire_link(gpointer) {
  state.link_retry_source = 0;
  if (state.capture == nullptr || state.ffmpeg == nullptr ||
      state.link != nullptr) {
    return G_SOURCE_REMOVE;
  }
  state.link_attempts++;
  const int output_port = pipewire_port_id(true, "gamescope");
  const int input_port = pipewire_port_id(false, "gst-launch-1.0");
  state.link_output_port = output_port;
  state.link_input_port = input_port;
  if (output_port < 0 || input_port < 0) {
    if (state.link_attempts < 30) {
      state.link_retry_source = g_timeout_add(200, start_pipewire_link, nullptr);
    }
    return G_SOURCE_REMOVE;
  }
  const std::vector<std::string> args = {
      "pw-link", "-L", std::to_string(output_port), std::to_string(input_port)};
  std::vector<const gchar*> argv;
  for (const auto& arg : args) argv.push_back(arg.c_str());
  argv.push_back(nullptr);

  state.link_error_path = data_path("link-stderr.log");
  g_file_set_contents(state.link_error_path.c_str(), "", 0, nullptr);
  GSubprocessLauncher* launcher =
      g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE);
  g_subprocess_launcher_setenv(launcher, "PIPEWIRE_REMOTE",
                               "pipewire-0-manager", TRUE);
  g_subprocess_launcher_set_stderr_file_path(launcher,
                                             state.link_error_path.c_str());
  GError* error = nullptr;
  state.link = g_subprocess_launcher_spawnv(launcher, argv.data(), &error);
  g_object_unref(launcher);
  if (state.link != nullptr) {
    write_log("Linking PipeWire ports " + std::to_string(output_port) + " -> " +
              std::to_string(input_port));
    g_subprocess_wait_async(state.link, nullptr, link_exited, nullptr);
  } else if (state.link_attempts < 30) {
    state.link_retry_source = g_timeout_add(200, start_pipewire_link, nullptr);
  }
  g_clear_error(&error);
  return G_SOURCE_REMOVE;
}

void stop_stream() {
  if (state.link_retry_source != 0) {
    g_source_remove(state.link_retry_source);
    state.link_retry_source = 0;
  }
  if (state.link != nullptr) {
    g_subprocess_force_exit(state.link);
    g_clear_object(&state.link);
  }
  state.link_established = false;
  if (state.capture != nullptr) {
    g_subprocess_force_exit(state.capture);
    g_clear_object(&state.capture);
  }
  if (state.ffmpeg != nullptr) {
    g_subprocess_force_exit(state.ffmpeg);
    g_clear_object(&state.ffmpeg);
  }
  state.command_display.clear();
}

void capture_exited(GObject* source, GAsyncResult* result, gpointer) {
  auto* process = G_SUBPROCESS(source);
  g_subprocess_wait_finish(process, result, nullptr);
  if (state.capture == process) {
    gchar* stderr_data = nullptr;
    gsize stderr_size = 0;
    g_file_get_contents(state.capture_error_path.c_str(), &stderr_data,
                        &stderr_size, nullptr);
    state.last_error = stderr_data ? stderr_data : "Gamescope PipeWire capture exited";
    g_free(stderr_data);
    state.exit_code = g_subprocess_get_if_exited(process)
                          ? g_subprocess_get_exit_status(process)
                          : -1;
    write_log("PipeWire capture exited with code " +
              std::to_string(state.exit_code) + ": " + state.last_error);
    g_clear_object(&state.capture);
    if (state.ffmpeg != nullptr) g_subprocess_force_exit(state.ffmpeg);
  }
}

void splice_finished(GObject* source, GAsyncResult* result, gpointer) {
  GError* error = nullptr;
  g_output_stream_splice_finish(G_OUTPUT_STREAM(source), result, &error);
  if (error != nullptr) {
    write_log(std::string("PipeWire stream splice ended: ") + error->message);
    g_clear_error(&error);
  }
}

void process_exited(GObject* source, GAsyncResult* result, gpointer) {
  auto* process = G_SUBPROCESS(source);
  GError* error = nullptr;
  g_subprocess_wait_finish(process, result, &error);
  if (state.ffmpeg == process) {
    gchar* stderr_data = nullptr;
    gsize stderr_size = 0;
    g_file_get_contents(state.ffmpeg_error_path.c_str(), &stderr_data,
                        &stderr_size, nullptr);
    if (stderr_data != nullptr && stderr_size > 0) {
      if (!state.last_error.empty()) state.last_error += "\nFFmpeg:\n";
      state.last_error += stderr_data;
    }
    g_free(stderr_data);
    if (error != nullptr && state.last_error.empty()) state.last_error = error->message;
    constexpr std::size_t kMaxErrorLength = 12000;
    if (state.last_error.size() > kMaxErrorLength) {
      state.last_error.erase(0, state.last_error.size() - kMaxErrorLength);
    }
    state.exit_code = g_subprocess_get_if_exited(process)
                          ? g_subprocess_get_exit_status(process)
                          : -1;
    write_log("FFmpeg exited with code " + std::to_string(state.exit_code) +
              ": " + state.last_error);
    if (state.capture != nullptr) {
      g_subprocess_force_exit(state.capture);
      g_clear_object(&state.capture);
    }
    g_clear_object(&state.ffmpeg);
    state.command_display.clear();
  }
  g_clear_error(&error);
}

void start_stream(JsonObject* config) {
  stop_stream();
  const auto args = build_ffmpeg_command(config);
  const std::string capture_mode = json_string(config, "capture_mode", "pipewire");
  state.last_error.clear();
  state.exit_code = 0;
  state.link_attempts = 0;
  state.capture_node_id = -1;
  state.link_output_port = -1;
  state.link_input_port = -1;
  state.link_retry_source = 0;
  state.link_established = false;

  std::vector<const gchar*> argv;
  std::ostringstream display;
  for (const auto& arg : args) {
    argv.push_back(arg.c_str());
    display << (display.tellp() > 0 ? " " : "") << arg;
  }
  argv.push_back(nullptr);

  GError* error = nullptr;
  if (capture_mode == "pipewire") {
    const auto capture_args = build_pipewire_command(config);
    std::vector<const gchar*> capture_argv;
    for (const auto& arg : capture_args) capture_argv.push_back(arg.c_str());
    capture_argv.push_back(nullptr);
    state.capture_error_path = data_path("capture-stderr.log");
    g_file_set_contents(state.capture_error_path.c_str(), "", 0, nullptr);
    GSubprocessLauncher* capture_launcher =
        g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE);
    g_subprocess_launcher_setenv(capture_launcher, "PIPEWIRE_REMOTE",
                                 "pipewire-0-manager", TRUE);
    g_subprocess_launcher_set_stderr_file_path(capture_launcher,
                                               state.capture_error_path.c_str());
    state.capture =
        g_subprocess_launcher_spawnv(capture_launcher, capture_argv.data(), &error);
    g_object_unref(capture_launcher);
    if (state.capture == nullptr) {
      const std::string message =
          error ? error->message : "Failed to start Gamescope PipeWire capture";
      g_clear_error(&error);
      throw std::runtime_error(message);
    }
    g_subprocess_wait_async(state.capture, nullptr, capture_exited, nullptr);
  }

  state.ffmpeg_error_path = data_path("ffmpeg-stderr.log");
  g_file_set_contents(state.ffmpeg_error_path.c_str(), "", 0, nullptr);
  GSubprocessLauncher* launcher = g_subprocess_launcher_new(
      static_cast<GSubprocessFlags>(
          (capture_mode == "pipewire" ? G_SUBPROCESS_FLAGS_STDIN_PIPE
                                       : G_SUBPROCESS_FLAGS_NONE) |
          G_SUBPROCESS_FLAGS_STDOUT_SILENCE));
  g_subprocess_launcher_set_stderr_file_path(launcher,
                                             state.ffmpeg_error_path.c_str());
  state.ffmpeg = g_subprocess_launcher_spawnv(launcher, argv.data(), &error);
  g_object_unref(launcher);
  if (state.ffmpeg == nullptr) {
    const std::string message = error ? error->message : "Failed to start FFmpeg";
    g_clear_error(&error);
    if (state.capture != nullptr) {
      g_subprocess_force_exit(state.capture);
      g_clear_object(&state.capture);
    }
    throw std::runtime_error(message);
  }
  if (capture_mode == "pipewire") {
    g_output_stream_splice_async(
        g_subprocess_get_stdin_pipe(state.ffmpeg),
        g_subprocess_get_stdout_pipe(state.capture),
        static_cast<GOutputStreamSpliceFlags>(
            G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
            G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET),
        G_PRIORITY_DEFAULT, nullptr, splice_finished, nullptr);
  }
  state.command_display = display.str();
  write_log("Starting FFmpeg: " + state.command_display);
  g_subprocess_wait_async(state.ffmpeg, nullptr, process_exited, nullptr);
  if (capture_mode == "pipewire") {
    state.link_retry_source = g_timeout_add(200, start_pipewire_link, nullptr);
  }
}

std::string escape_json(const std::string& text) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string output;
  for (const unsigned char value : text) {
    if (value == '\\' || value == '"') {
      output += '\\';
      output += static_cast<char>(value);
    } else if (value == '\n') {
      output += "\\n";
    } else if (value == '\r') {
      output += "\\r";
    } else if (value == '\t') {
      output += "\\t";
    } else if (value < 0x20) {
      output += "\\u00";
      output += hex[value >> 4];
      output += hex[value & 0x0f];
    } else {
      output += static_cast<char>(value);
    }
  }
  return output;
}

std::string status_json() {
  return std::string("{\"running\":") + (state.ffmpeg ? "true" : "false") +
         ",\"link_running\":" +
         (state.ffmpeg && (state.link || state.link_established) ? "true"
                                                                 : "false") +
         ",\"link_attempts\":" + std::to_string(state.link_attempts) +
         ",\"capture_node_id\":" + std::to_string(state.capture_node_id) +
         ",\"link_output_port\":" + std::to_string(state.link_output_port) +
         ",\"link_input_port\":" + std::to_string(state.link_input_port) +
         ",\"last_error\":\"" + escape_json(state.last_error) +
         "\",\"exit_code\":" + std::to_string(state.exit_code) +
         ",\"protocol_version\":" + std::to_string(kProtocolVersion) +
         ",\"app_version\":\"" + kAppVersion + "\"}";
}

std::string diagnostics_json() {
  std::string capture_error = file_contents(state.capture_error_path);
  const std::string link_error = file_contents(state.link_error_path);
  if (!link_error.empty()) {
    capture_error +=
        (capture_error.empty() ? "" : "\n") + std::string("PipeWire link:\n") +
        link_error;
  }
  return std::string("{\"pipewire_nodes\":\"") +
         escape_json(pipewire_command_output({"pw-cli", "list-objects", "Node"})) +
         "\",\"pipewire_ports\":\"" +
         escape_json(pipewire_command_output({"pw-link", "-iolI"})) + "\"," +
         "\"capture_stderr\":\"" +
         escape_json(capture_error) +
         "\",\"ffmpeg_stderr\":\"" +
         escape_json(file_contents(state.ffmpeg_error_path)) + "\"}";
}

void send_response(GOutputStream* output, int status, const std::string& body) {
  const char* status_text = status == 200 ? "OK" : status == 404 ? "Not Found"
                                                               : "Bad Request";
  std::ostringstream response;
  response << "HTTP/1.1 " << status << " " << status_text << "\r\n"
           << "Content-Type: application/json\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Connection: close\r\n\r\n"
           << body;
  const std::string value = response.str();
  g_output_stream_write_all(output, value.data(), value.size(), nullptr, nullptr,
                            nullptr);
}

std::string read_request_body(GDataInputStream* input, gsize content_length) {
  std::string body(content_length, '\0');
  if (content_length > 0) {
    gsize bytes_read = 0;
    if (!g_input_stream_read_all(G_INPUT_STREAM(input), body.data(),
                                 content_length, &bytes_read, nullptr, nullptr)) {
      throw std::runtime_error("Failed to read the request body");
    }
    body.resize(bytes_read);
  }
  return body;
}

std::string read_chunked_request_body(GDataInputStream* input) {
  std::string body;
  while (true) {
    gsize length = 0;
    gchar* line = g_data_input_stream_read_line(input, &length, nullptr, nullptr);
    if (line == nullptr) throw std::runtime_error("Invalid chunked request body");
    std::string size_line(line, length);
    g_free(line);
    if (!size_line.empty() && size_line.back() == '\r') size_line.pop_back();

    const auto extension = size_line.find(';');
    if (extension != std::string::npos) size_line.resize(extension);
    const gsize chunk_size = std::stoul(size_line, nullptr, 16);
    if (chunk_size == 0) {
      while (true) {
        line = g_data_input_stream_read_line(input, &length, nullptr, nullptr);
        if (line == nullptr || length == 0 ||
            (length == 1 && line[0] == '\r')) {
          g_free(line);
          return body;
        }
        g_free(line);
      }
    }

    body += read_request_body(input, chunk_size);
    line = g_data_input_stream_read_line(input, &length, nullptr, nullptr);
    if (line == nullptr || (length != 0 && !(length == 1 && line[0] == '\r'))) {
      g_free(line);
      throw std::runtime_error("Invalid chunk separator");
    }
    g_free(line);
  }
}

std::string encoder_capabilities() {
  gchar* stdout_data = nullptr;
  GError* error = nullptr;
  if (!g_spawn_command_line_sync("ffmpeg -hide_banner -encoders", &stdout_data,
                                 nullptr, nullptr, &error)) {
    const std::string message = error ? error->message : "FFmpeg is unavailable";
    g_clear_error(&error);
    return "{\"error\":\"" + escape_json(message) + "\"}";
  }
  const std::string output = stdout_data ? stdout_data : "";
  g_free(stdout_data);
  const std::vector<std::string> names = {
      "libx264", "h264_vaapi", "libx265", "hevc_vaapi",
      "libsvtav1", "libaom-av1", "av1_vaapi"};
  std::ostringstream json;
  json << "{\"encoders\":[";
  bool first = true;
  for (const auto& name : names) {
    if (output.find(name) != std::string::npos) {
      json << (first ? "" : ",") << "\"" << name << "\"";
      first = false;
    }
  }
  json << "],\"drm_devices\":[";
  first = true;
  for (const auto& path : device_paths("card")) {
    json << (first ? "" : ",") << "\"" << escape_json(path) << "\"";
    first = false;
  }
  json << "],\"vaapi_devices\":[";
  first = true;
  for (const auto& path : device_paths("renderD")) {
    json << (first ? "" : ",") << "\"" << escape_json(path) << "\"";
    first = false;
  }
  json << "],\"pipewire_manager_available\":"
       << (pipewire_manager_available() ? "true" : "false")
       << ",\"gstreamer_va\":\""
       << escape_json(pipewire_command_output({"gst-inspect-1.0", "va"}))
       << "\"}";
  return json.str();
}

gboolean handle_connection(GSocketService*, GSocketConnection* connection,
                           GObject*, gpointer) {
  GInputStream* raw_input =
      g_io_stream_get_input_stream(G_IO_STREAM(connection));
  GOutputStream* output =
      g_io_stream_get_output_stream(G_IO_STREAM(connection));
  GDataInputStream* input = g_data_input_stream_new(raw_input);

  try {
    gsize length = 0;
    gchar* request_line =
        g_data_input_stream_read_line(input, &length, nullptr, nullptr);
    if (request_line == nullptr) throw std::runtime_error("Empty HTTP request");
    std::istringstream request(request_line);
    g_free(request_line);
    std::string method;
    std::string path;
    request >> method >> path;

    gsize content_length = 0;
    bool chunked = false;
    while (true) {
      gchar* header = g_data_input_stream_read_line(input, &length, nullptr, nullptr);
      if (header == nullptr || length == 0 ||
          (length == 1 && header[0] == '\r')) {
        g_free(header);
        break;
      }
      const std::string value = header;
      g_free(header);
      std::string lower = value;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                     });
      if (lower.rfind("content-length:", 0) == 0) {
        content_length = std::stoul(value.substr(value.find(':') + 1));
      } else if (lower.rfind("transfer-encoding:", 0) == 0 &&
                 lower.find("chunked") != std::string::npos) {
        chunked = true;
      }
    }

    const std::string body =
        chunked ? read_chunked_request_body(input)
                : read_request_body(input, content_length);
    if (method == "GET" && path == "/api/status") {
      send_response(output, 200, status_json());
    } else if (method == "GET" && path == "/api/capabilities") {
      send_response(output, 200, encoder_capabilities());
    } else if (method == "GET" && path == "/api/diagnostics") {
      send_response(output, 200, diagnostics_json());
    } else if (method == "POST" && path == "/api/stop") {
      stop_stream();
      send_response(output, 200, "{\"running\":false}");
    } else if (method == "POST" && path == "/api/start") {
      JsonParser* parser = json_parser_new();
      GError* error = nullptr;
      if (!json_parser_load_from_data(parser, body.c_str(), body.size(), &error)) {
        const std::string message = error ? error->message : "Invalid JSON";
        g_clear_error(&error);
        g_object_unref(parser);
        throw std::runtime_error(message);
      }
      JsonNode* root = json_parser_get_root(parser);
      if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        throw std::runtime_error("The request body must be a JSON object");
      }
      start_stream(json_node_get_object(root));
      g_object_unref(parser);
      send_response(output, 200, "{\"running\":true}");
    } else {
      send_response(output, 404, "{\"error\":\"not found\"}");
    }
  } catch (const std::exception& error) {
    send_response(output, 400,
                  "{\"error\":\"" + escape_json(error.what()) + "\"}");
  }

  g_object_unref(input);
  g_io_stream_close(G_IO_STREAM(connection), nullptr, nullptr);
  return TRUE;
}

}  // namespace

int main() {
  write_log(std::string("Starting Deck Screen Share ") + kAppVersion +
            " in headless mode");

  GError* error = nullptr;
  state.service = g_socket_service_new();
  if (!g_socket_listener_add_inet_port(G_SOCKET_LISTENER(state.service),
                                       kAgentPort, nullptr, &error)) {
    g_printerr("Cannot listen on port %u: %s\n", kAgentPort,
               error ? error->message : "unknown error");
    write_log(std::string("Cannot listen on port ") + std::to_string(kAgentPort) +
              ": " + (error ? error->message : "unknown error"));
    g_clear_error(&error);
    return 1;
  }
  g_signal_connect(state.service, "incoming", G_CALLBACK(handle_connection),
                   nullptr);
  g_socket_service_start(state.service);

  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
  g_clear_object(&state.service);
  return 0;
}
