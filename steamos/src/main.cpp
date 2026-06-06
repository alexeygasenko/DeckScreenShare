#include <gio/gio.h>
#include <gtk/gtk.h>
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
constexpr int kProtocolVersion = 8;
constexpr const char* kAppVersion = "0.1.10";

struct AppState {
  GtkWidget* status_label = nullptr;
  GtkWidget* command_label = nullptr;
  GSocketService* service = nullptr;
  GSubprocess* ffmpeg = nullptr;
  GSubprocess* capture = nullptr;
  std::string command_display;
  std::string last_error;
  std::string ffmpeg_error_path;
  std::string capture_error_path;
  int exit_code = 0;
  int link_attempts = 0;
  bool gtk_available = false;
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

std::string pipewire_nodes() {
  const std::vector<std::string> args = {"pw-cli", "list-objects", "Node"};
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
    const std::string message = error ? error->message : "Failed to start pw-cli";
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
  constexpr std::size_t kMaxOutputLength = 30000;
  if (output.size() > kMaxOutputLength) output.resize(kMaxOutputLength);
  return output;
}

std::vector<std::string> build_ffmpeg_command(JsonObject* config) {
  const std::string codec = json_string(config, "codec", "h264");
  const std::string backend = json_string(config, "backend", "software");
  const std::string capture_mode = json_string(config, "capture_mode", "pipewire");
  const std::string receiver_host = json_string(config, "receiver_host");
  const int srt_port = json_int(config, "srt_port", 9000, 1, 65535);
  const int fps = json_int(config, "fps", 60, 1, 240);
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
    append(args, {"-thread_queue_size", "1024", "-f", "yuv4mpegpipe", "-i",
                  "pipe:0"});
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
    append(args, {"-vf", capture_mode == "kmsgrab"
                             ? "hwmap=derive_device=vaapi,scale_vaapi=format=nv12"
                             : "format=nv12,hwupload"});
  } else {
    if (capture_mode == "kmsgrab") {
      append(args, {"-vf", "hwdownload,format=bgr0"});
    }
    if (encoder == "libx264" || encoder == "libx265") {
      append(args, {"-preset", "veryfast", "-tune", "zerolatency"});
    } else if (encoder == "libsvtav1") {
      append(args, {"-preset", "10"});
    }
  }

  const std::string video_rate = std::to_string(video_bitrate) + "k";
  const std::string receiver_url =
      "tcp://" + receiver_host + ":" + std::to_string(srt_port);
  append(args, {"-map", "0:v:0", "-map", "1:a:0", "-c:v", encoder, "-b:v",
                video_rate, "-maxrate", video_rate, "-bufsize",
                std::to_string(video_bitrate * 2) + "k", "-g",
                std::to_string(fps * 2), "-c:a", "libopus", "-b:a",
                std::to_string(audio_bitrate) + "k", "-ar", "48000", "-f",
                "matroska", receiver_url});
  return args;
}

std::vector<std::string> build_pipewire_command(JsonObject* config) {
  const int fps = json_int(config, "fps", 60, 1, 240);
  return {"gst-launch-1.0", "-q", "pipewiresrc", "target-object=gamescope",
          "do-timestamp=true", "min-buffers=8", "!", "glupload", "!",
          "glcolorconvert", "!", "gldownload", "!", "video/x-raw,format=RGBA",
          "!", "videoconvert", "!", "videorate", "!",
          "video/x-raw,format=I420,framerate=" + std::to_string(fps) + "/1",
          "!", "y4menc", "!", "fdsink", "fd=1"};
}

gboolean link_pipewire_nodes(gpointer) {
  if (state.capture == nullptr || state.ffmpeg == nullptr) return G_SOURCE_REMOVE;
  state.link_attempts++;
  const std::vector<std::string> args = {
      "pw-link", "-L", "gamescope", "gst-launch-1.0"};
  std::vector<const gchar*> argv;
  for (const auto& arg : args) argv.push_back(arg.c_str());
  argv.push_back(nullptr);

  GSubprocessLauncher* launcher = g_subprocess_launcher_new(
      static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                    G_SUBPROCESS_FLAGS_STDERR_SILENCE));
  g_subprocess_launcher_setenv(launcher, "PIPEWIRE_REMOTE",
                               "pipewire-0-manager", TRUE);
  GError* error = nullptr;
  GSubprocess* process =
      g_subprocess_launcher_spawnv(launcher, argv.data(), &error);
  g_object_unref(launcher);
  if (process != nullptr) {
    g_subprocess_wait(process, nullptr, nullptr);
    const bool linked =
        g_subprocess_get_if_exited(process) &&
        g_subprocess_get_exit_status(process) == 0;
    g_object_unref(process);
    if (linked) {
      write_log("Linked Gamescope PipeWire output to GStreamer input");
      return G_SOURCE_REMOVE;
    }
  }
  g_clear_error(&error);
  return state.link_attempts < 50 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

void refresh_status() {
  if (state.status_label == nullptr || state.command_label == nullptr) return;
  const bool running = state.ffmpeg != nullptr;
  gtk_label_set_text(GTK_LABEL(state.status_label),
                     running ? "Status: streaming" : "Status: ready");
  gtk_label_set_text(GTK_LABEL(state.command_label),
                     state.command_display.c_str());
}

void stop_stream() {
  if (state.capture != nullptr) {
    g_subprocess_force_exit(state.capture);
    g_clear_object(&state.capture);
  }
  if (state.ffmpeg != nullptr) {
    g_subprocess_force_exit(state.ffmpeg);
    g_clear_object(&state.ffmpeg);
  }
  state.command_display.clear();
  refresh_status();
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
    refresh_status();
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
  refresh_status();
  g_subprocess_wait_async(state.ffmpeg, nullptr, process_exited, nullptr);
  if (capture_mode == "pipewire") {
    g_timeout_add(100, link_pipewire_nodes, nullptr);
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
         ",\"last_error\":\"" + escape_json(state.last_error) +
         "\",\"exit_code\":" + std::to_string(state.exit_code) +
         ",\"protocol_version\":" + std::to_string(kProtocolVersion) +
         ",\"app_version\":\"" + kAppVersion + "\"}";
}

std::string diagnostics_json() {
  return "{\"pipewire_nodes\":\"" + escape_json(pipewire_nodes()) +
         "\",\"capture_stderr\":\"" +
         escape_json(file_contents(state.capture_error_path)) +
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
       << (pipewire_manager_available() ? "true" : "false") << "}";
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

void stop_clicked(GtkButton*, gpointer) { stop_stream(); }

void hide_clicked(GtkButton*, gpointer data) {
  gtk_window_iconify(GTK_WINDOW(data));
}

gboolean window_delete_requested(GtkWidget* window, GdkEvent*, gpointer) {
  gtk_widget_hide(window);
  write_log("Window hidden; agent continues running");
  return TRUE;
}

void window_destroyed(GtkWidget*, gpointer) {
  state.status_label = nullptr;
  state.command_label = nullptr;
  write_log("Window destroyed; agent continues running in headless mode");
}

void quit_clicked(GtkButton*, gpointer) {
  write_log("Agent stopped by user");
  stop_stream();
  if (state.service != nullptr) g_socket_service_stop(state.service);
  gtk_main_quit();
}

GtkWidget* create_window() {
  GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  const std::string window_title =
      "Deck Screen Share " + std::string(kAppVersion);
  gtk_window_set_title(GTK_WINDOW(window), window_title.c_str());
  gtk_window_set_default_size(GTK_WINDOW(window), 560, 320);
  gtk_container_set_border_width(GTK_CONTAINER(window), 22);
  g_signal_connect(window, "delete-event",
                   G_CALLBACK(window_delete_requested), nullptr);
  g_signal_connect(window, "destroy", G_CALLBACK(window_destroyed), nullptr);

  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
  gtk_container_add(GTK_CONTAINER(window), box);

  GtkWidget* title = gtk_label_new(nullptr);
  gtk_label_set_markup(GTK_LABEL(title),
                       ("<span size='x-large' weight='bold'>Deck Screen Share " +
                        std::string(kAppVersion) + "</span>").c_str());
  gtk_label_set_xalign(GTK_LABEL(title), 0);
  gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

  GtkWidget* description = gtk_label_new(
      "Ready to accept commands from the Windows receiver.\n"
      "Keep this application running, then launch the game you want to capture.");
  gtk_label_set_xalign(GTK_LABEL(description), 0);
  gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
  gtk_box_pack_start(GTK_BOX(box), description, FALSE, FALSE, 0);

  state.status_label = gtk_label_new("Status: ready");
  gtk_label_set_xalign(GTK_LABEL(state.status_label), 0);
  gtk_box_pack_start(GTK_BOX(box), state.status_label, FALSE, FALSE, 0);

  state.command_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(state.command_label), 0);
  gtk_label_set_line_wrap(GTK_LABEL(state.command_label), TRUE);
  gtk_label_set_selectable(GTK_LABEL(state.command_label), TRUE);
  gtk_box_pack_start(GTK_BOX(box), state.command_label, TRUE, TRUE, 0);

  GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget* stop = gtk_button_new_with_label("Stop stream");
  g_signal_connect(stop, "clicked", G_CALLBACK(stop_clicked), nullptr);
  gtk_box_pack_start(GTK_BOX(buttons), stop, FALSE, FALSE, 0);
  GtkWidget* hide = gtk_button_new_with_label("Hide window");
  g_signal_connect(hide, "clicked", G_CALLBACK(hide_clicked), window);
  gtk_box_pack_start(GTK_BOX(buttons), hide, FALSE, FALSE, 0);
  GtkWidget* quit = gtk_button_new_with_label("Quit agent");
  g_signal_connect(quit, "clicked", G_CALLBACK(quit_clicked), nullptr);
  gtk_box_pack_start(GTK_BOX(buttons), quit, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(box), buttons, FALSE, FALSE, 0);
  return window;
}

}  // namespace

int main(int argc, char** argv) {
  state.gtk_available = gtk_init_check(&argc, &argv);
  write_log(std::string("Starting Deck Screen Share ") + kAppVersion +
            (state.gtk_available ? " with GTK display" : " in headless mode"));

  GError* error = nullptr;
  state.service = g_socket_service_new();
  if (!g_socket_listener_add_inet_port(G_SOCKET_LISTENER(state.service),
                                       kAgentPort, nullptr, &error)) {
    g_printerr("Cannot listen on port %u: %s\n", kAgentPort,
               error ? error->message : "unknown error");
    write_log(std::string("Cannot listen on port ") + std::to_string(kAgentPort) +
              ": " + (error ? error->message : "unknown error"));
    if (state.gtk_available) {
      GtkWidget* dialog = gtk_message_dialog_new(
          nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
          "Cannot listen on port %u: %s", kAgentPort,
          error ? error->message : "unknown error");
      gtk_dialog_run(GTK_DIALOG(dialog));
      gtk_widget_destroy(dialog);
    }
    g_clear_error(&error);
    return 1;
  }
  g_signal_connect(state.service, "incoming", G_CALLBACK(handle_connection),
                   nullptr);
  g_socket_service_start(state.service);

  if (state.gtk_available) {
    GtkWidget* window = create_window();
    gtk_widget_show_all(window);
    gtk_main();
  } else {
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
  }
  g_clear_object(&state.service);
  return 0;
}
