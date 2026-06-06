#include <gio/gio.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

#include <algorithm>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr guint kAgentPort = 8765;

struct AppState {
  GtkWidget* status_label = nullptr;
  GtkWidget* command_label = nullptr;
  GSocketService* service = nullptr;
  GSubprocess* ffmpeg = nullptr;
  std::string command_display;
};

AppState state;

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

std::vector<std::string> build_ffmpeg_command(JsonObject* config) {
  const std::string codec = json_string(config, "codec", "h264");
  const std::string backend = json_string(config, "backend", "software");
  const std::string capture_mode = json_string(config, "capture_mode", "kmsgrab");
  const std::string receiver_host = json_string(config, "receiver_host");
  const int srt_port = json_int(config, "srt_port", 9000, 1, 65535);
  const int fps = json_int(config, "fps", 60, 1, 240);
  const int video_bitrate =
      json_int(config, "video_bitrate_kbps", 8000, 250, 100000);
  const int audio_bitrate =
      json_int(config, "audio_bitrate_kbps", 160, 32, 1024);
  const int latency = json_int(config, "latency_ms", 120, 20, 5000);

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
  if (capture_mode == "kmsgrab") {
    append(args, {"-f", "kmsgrab", "-device",
                  json_string(config, "drm_device", "/dev/dri/card0"),
                  "-framerate", std::to_string(fps), "-i", "-"});
  } else if (capture_mode == "x11grab") {
    append(args, {"-f", "x11grab", "-draw_mouse", "1", "-framerate",
                  std::to_string(fps), "-video_size",
                  json_string(config, "size", "1280x800"), "-i",
                  json_string(config, "display", ":0.0")});
  } else {
    throw std::runtime_error("capture_mode must be kmsgrab or x11grab");
  }

  append(args, {"-thread_queue_size", "1024", "-f", "pulse", "-i",
                json_string(config, "audio_source", "@DEFAULT_MONITOR@")});

  const std::string encoder = encoders.at(codec).at(backend);
  if (backend == "vaapi") {
    append(args, {"-vaapi_device",
                  json_string(config, "vaapi_device", "/dev/dri/renderD128")});
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
  const std::string srt_url =
      "srt://" + receiver_host + ":" + std::to_string(srt_port) +
      "?mode=caller&transtype=live&latency=" + std::to_string(latency * 1000);
  append(args, {"-map", "0:v:0", "-map", "1:a:0", "-c:v", encoder, "-b:v",
                video_rate, "-maxrate", video_rate, "-bufsize",
                std::to_string(video_bitrate * 2) + "k", "-g",
                std::to_string(fps * 2), "-c:a", "libopus", "-b:a",
                std::to_string(audio_bitrate) + "k", "-ar", "48000", "-f",
                "matroska", srt_url});
  return args;
}

void refresh_status() {
  const bool running = state.ffmpeg != nullptr;
  gtk_label_set_text(GTK_LABEL(state.status_label),
                     running ? "Status: streaming" : "Status: ready");
  gtk_label_set_text(GTK_LABEL(state.command_label),
                     state.command_display.c_str());
}

void stop_stream() {
  if (state.ffmpeg != nullptr) {
    g_subprocess_force_exit(state.ffmpeg);
    g_clear_object(&state.ffmpeg);
  }
  state.command_display.clear();
  refresh_status();
}

void process_exited(GObject* source, GAsyncResult* result, gpointer) {
  auto* process = G_SUBPROCESS(source);
  g_subprocess_wait_finish(process, result, nullptr);
  if (state.ffmpeg == process) {
    g_clear_object(&state.ffmpeg);
    state.command_display.clear();
    refresh_status();
  }
}

void start_stream(JsonObject* config) {
  stop_stream();
  const auto args = build_ffmpeg_command(config);

  std::vector<const gchar*> argv;
  std::ostringstream display;
  for (const auto& arg : args) {
    argv.push_back(arg.c_str());
    display << (display.tellp() > 0 ? " " : "") << arg;
  }
  argv.push_back(nullptr);

  GError* error = nullptr;
  state.ffmpeg = g_subprocess_newv(
      argv.data(),
      G_SUBPROCESS_FLAGS_STDOUT_SILENCE,
      &error);
  if (state.ffmpeg == nullptr) {
    const std::string message = error ? error->message : "Failed to start FFmpeg";
    g_clear_error(&error);
    throw std::runtime_error(message);
  }
  state.command_display = display.str();
  refresh_status();
  g_subprocess_wait_async(state.ffmpeg, nullptr, process_exited, nullptr);
}

std::string escape_json(const std::string& text) {
  std::string output;
  for (const char value : text) {
    if (value == '\\' || value == '"') output += '\\';
    if (value == '\n') {
      output += "\\n";
    } else {
      output += value;
    }
  }
  return output;
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
  json << "]}";
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
    while (true) {
      gchar* header = g_data_input_stream_read_line(input, &length, nullptr, nullptr);
      if (header == nullptr || length == 0 ||
          (length == 1 && header[0] == '\r')) {
        g_free(header);
        break;
      }
      const std::string value = header;
      g_free(header);
      if (value.rfind("Content-Length:", 0) == 0) {
        content_length = std::stoul(value.substr(15));
      }
    }

    const std::string body = read_request_body(input, content_length);
    if (method == "GET" && path == "/api/status") {
      send_response(output, 200,
                    std::string("{\"running\":") +
                        (state.ffmpeg ? "true" : "false") + "}");
    } else if (method == "GET" && path == "/api/capabilities") {
      send_response(output, 200, encoder_capabilities());
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

void window_destroyed(GtkWidget*, gpointer) {
  stop_stream();
  if (state.service != nullptr) g_socket_service_stop(state.service);
  gtk_main_quit();
}

GtkWidget* create_window() {
  GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window), "Deck Screen Share");
  gtk_window_set_default_size(GTK_WINDOW(window), 560, 320);
  gtk_container_set_border_width(GTK_CONTAINER(window), 22);
  g_signal_connect(window, "destroy", G_CALLBACK(window_destroyed), nullptr);

  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
  gtk_container_add(GTK_CONTAINER(window), box);

  GtkWidget* title = gtk_label_new(nullptr);
  gtk_label_set_markup(GTK_LABEL(title),
                       "<span size='x-large' weight='bold'>Deck Screen Share</span>");
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
  gtk_box_pack_end(GTK_BOX(box), buttons, FALSE, FALSE, 0);
  return window;
}

}  // namespace

int main(int argc, char** argv) {
  gtk_init(&argc, &argv);

  GError* error = nullptr;
  state.service = g_socket_service_new();
  if (!g_socket_listener_add_inet_port(G_SOCKET_LISTENER(state.service),
                                       kAgentPort, nullptr, &error)) {
    g_printerr("Cannot listen on port %u: %s\n", kAgentPort,
               error ? error->message : "unknown error");
    g_clear_error(&error);
    return 1;
  }
  g_signal_connect(state.service, "incoming", G_CALLBACK(handle_connection),
                   nullptr);
  g_socket_service_start(state.service);

  GtkWidget* window = create_window();
  gtk_widget_show_all(window);
  gtk_main();
  g_clear_object(&state.service);
  return 0;
}
