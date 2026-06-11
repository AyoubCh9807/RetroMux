#include "libretro.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <drogon/WebSocketConnection.h>
#include <drogon/drogon.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <oneapi/tbb/concurrent_vector.h>
#include <rtc/common.hpp>
#include <rtc/peerconnection.hpp>
#include <rtc/rtc.hpp>
#include <sys/types.h>
#include <tbb/concurrent_vector.h>
#include <thread>
#include <vector>

// High-performance logging library
#include <spdlog/spdlog.h>

// ============================================================================
// 1. GLOBAL CORE INPUT STATE
// ============================================================================
struct ControllerState {
  int16_t a = 0, b = 0, select = 0, start = 0;
  int16_t up = 0, down = 0, left = 0, right = 0;
};

// Supporting up to 2 players for WebRTC remote play injection
ControllerState g_players[2];

extern "C" {

// Public API to inject inputs (e.g., from local keyboard or remote WebRTC
// DataChannel)
void retro_mux_set_input(int player_port, int button_id, bool is_pressed) {
  if (player_port < 0 || player_port > 1)
    return;

  int16_t state = is_pressed ? 1 : 0;
  switch (button_id) {
  case RETRO_DEVICE_ID_JOYPAD_A:
    g_players[player_port].a = state;
    break;
  case RETRO_DEVICE_ID_JOYPAD_B:
    g_players[player_port].b = state;
    break;
  case RETRO_DEVICE_ID_JOYPAD_SELECT:
    g_players[player_port].select = state;
    break;
  case RETRO_DEVICE_ID_JOYPAD_START:
    g_players[player_port].start = state;
    break;
  case RETRO_DEVICE_ID_JOYPAD_UP:
    g_players[player_port].up = state;
    break;
  case RETRO_DEVICE_ID_JOYPAD_DOWN:
    g_players[player_port].down = state;
    break;
  case RETRO_DEVICE_ID_JOYPAD_LEFT:
    g_players[player_port].left = state;
    break;
  case RETRO_DEVICE_ID_JOYPAD_RIGHT:
    g_players[player_port].right = state;
    break;
  }
}

// ============================================================================
// 2. MANDATORY LIBRETRO CALLBACK IMPLEMENTATIONS
// ============================================================================

unsigned g_bytes_per_pixel = 2;

bool cb_environment(unsigned cmd, void *data) {
  switch (cmd) {
  case RETRO_ENVIRONMENT_GET_CAN_DUPE:
    if (data)
      *static_cast<bool *>(data) = true;
    return true;

  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
    if (!data)
      return false;

    const auto *fmt = static_cast<const enum retro_pixel_format *>(data);

    if (*fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
      g_bytes_per_pixel = 4;
    } else {
      g_bytes_per_pixel = 2;
    }
    return true;
  }

  default:
    return false;
  }
}

tbb::detail::d1::concurrent_vector<uint8_t> g_back_buffer;
std::atomic<bool> g_is_writing{false};

std::shared_ptr<rtc::PeerConnection> g_peer_connection = nullptr;
std::shared_ptr<rtc::DataChannel> g_rtc_channel = nullptr;

void cb_video_refresh(const void *data, unsigned width, unsigned height,
                      size_t pitch) {
  if (!data)
    return;
  g_is_writing.store(true, std::memory_order_relaxed);

  g_back_buffer.resize(width * height * g_bytes_per_pixel);

  const uint8_t *start = static_cast<const uint8_t *>(data);

  // Using a safe cast here to handle C++23 size_t literal safely across old and
  // new GCC/Clang variants
  for (size_t row = 0; row < height; row++) {
    auto begin = start + pitch * row;
    std::copy(begin, begin + width * g_bytes_per_pixel,
              g_back_buffer.begin() + row * width * g_bytes_per_pixel);
  }
  g_is_writing.store(false, std::memory_order_release);

  // If a browser has established a connection, stream the pixel array down the
  // pipe
  if (g_rtc_channel && g_rtc_channel->isOpen() && !g_back_buffer.empty()) {
    const std::byte *payload =
        reinterpret_cast<const std::byte *>(&g_back_buffer[0]);
    size_t total_bytes = g_back_buffer.size();
    g_rtc_channel->send(payload, total_bytes);
  }
}

void cb_audio_sample(int16_t left, int16_t right) {}

size_t cb_audio_sample_batch(const int16_t *data, size_t frames) {
  return frames;
}

void cb_input_poll() {}

int16_t cb_input_state(unsigned port, unsigned device, unsigned index,
                       unsigned id) {
  if (device != RETRO_DEVICE_JOYPAD)
    return 0;

  // TEMPORARY TEST: Combine both input slots onto Port 0 (Player 1)
  if (port == 0) {
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_A:
      return g_players[0].a || g_players[1].a;
    case RETRO_DEVICE_ID_JOYPAD_B:
      return g_players[0].b || g_players[1].b;
    case RETRO_DEVICE_ID_JOYPAD_SELECT:
      return g_players[0].select || g_players[1].select;
    case RETRO_DEVICE_ID_JOYPAD_START:
      return g_players[0].start || g_players[1].start;
    case RETRO_DEVICE_ID_JOYPAD_UP:
      return g_players[0].up || g_players[1].up;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:
      return g_players[0].down || g_players[1].down;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:
      return g_players[0].left || g_players[1].left;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:
      return g_players[0].right || g_players[1].right;
    }
  }
  return 0;
}

} // extern "C"

// ============================================================================
// 3. MAIN HOST EXECUTION ENGINE
// ============================================================================
int main() {
  // Initialize spdlog performance parameters
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
  spdlog::info("Initializing RetroMux host system...");

  // 1. Dynamically load the target Libretro core shared library
  const char *core_path = "./gambatte_libretro.so";
  void *core_handle = dlopen(core_path, RTLD_LAZY);
  if (!core_handle) {
    spdlog::error("Failed to load core! Error: {}", dlerror());
    return 1;
  }

  // 2. Resolve mandatory system function pointers
  typedef void (*retro_void_fn)();
  auto retro_init = (retro_void_fn)dlsym(core_handle, "retro_init");
  auto retro_deinit = (retro_void_fn)dlsym(core_handle, "retro_deinit");
  auto retro_run = (retro_void_fn)dlsym(core_handle, "retro_run");
  auto retro_unload_game =
      (retro_void_fn)dlsym(core_handle, "retro_unload_game");

  typedef void (*set_env_fn)(retro_environment_t);
  typedef void (*set_video_fn)(retro_video_refresh_t);
  typedef void (*set_audio_fn)(retro_audio_sample_t);
  typedef void (*set_audio_batch_fn)(retro_audio_sample_batch_t);
  typedef void (*set_input_poll_fn)(retro_input_poll_t);
  typedef void (*set_input_state_fn)(retro_input_state_t);
  typedef bool (*load_game_fn)(const struct retro_game_info *);

  auto retro_set_environment =
      (set_env_fn)dlsym(core_handle, "retro_set_environment");
  auto retro_set_video_refresh =
      (set_video_fn)dlsym(core_handle, "retro_set_video_refresh");
  auto retro_set_audio =
      (set_audio_fn)dlsym(core_handle, "retro_set_audio_sample");
  auto retro_set_audio_batch =
      (set_audio_batch_fn)dlsym(core_handle, "retro_set_audio_sample_batch");
  auto retro_set_input_poll =
      (set_input_poll_fn)dlsym(core_handle, "retro_set_input_poll");
  auto retro_set_input_state =
      (set_input_state_fn)dlsym(core_handle, "retro_set_input_state");
  auto retro_load_game = (load_game_fn)dlsym(core_handle, "retro_load_game");

  // 3. Bind host callbacks to core
  retro_set_environment(cb_environment);
  retro_set_video_refresh(cb_video_refresh);
  retro_set_audio(cb_audio_sample);
  retro_set_audio_batch(cb_audio_sample_batch);
  retro_set_input_poll(cb_input_poll);
  retro_set_input_state(cb_input_state);

  // 4. Engine system initialization
  retro_init();

  // 5. Load the target game ROM into a contiguous memory buffer
  std::string rom_path = "test_game.gb";
  std::ifstream file(rom_path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    spdlog::error("Error: Could not find '{}'", rom_path);
    retro_deinit();
    dlclose(core_handle);
    return 1;
  }

  size_t size = file.tellg();
  file.seekg(0, std::ios::beg);
  std::vector<uint8_t> rom_buffer(size);
  file.read(reinterpret_cast<char *>(rom_buffer.data()), size);
  file.close();

  // 6. Pass ownership descriptors of the memory buffer to the core
  struct retro_game_info game_info = {rom_path.c_str(), rom_buffer.data(), size,
                                      nullptr};
  if (!retro_load_game(&game_info)) {
    spdlog::error("Error: Core rejected or failed to parse the game ROM!");
    retro_deinit();
    dlclose(core_handle);
    return 1;
  }

  // 7. Core-managed execution loop running on its own thread
  std::thread emulator_thread([&]() {
    const std::chrono::duration<double> frame_target_duration(1.0 / 60.0);
    auto start_time = std::chrono::high_resolution_clock::now();

    while (true) {
      auto current_time = std::chrono::high_resolution_clock::now();
      auto elapsed = current_time - start_time;

      if (elapsed >= frame_target_duration) {
        retro_run();
        start_time = current_time;
      } else {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
      }
    }
  });
  emulator_thread.detach();

  // Force inject CORS headers on all outbound responses handled by the app
  drogon::app().registerPostHandlingAdvice(
      [](const drogon::HttpRequestPtr &req,
         const drogon::HttpResponsePtr &resp) {
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Headers", "*");
        resp->addHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
      });

  // 9. Drogon HTTP Signal Negotiation Setup (Accepts both OPTIONS and POST
  // requests cleanly)
  // 9. Drogon HTTP Signal Negotiation Setup (Accepts both OPTIONS and POST
  // requests cleanly)
  drogon::app().registerHandler(
      "/negotiate",
      [](const drogon::HttpRequestPtr &req,
         std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        // Inline CORS Check: Handle browser preflight OPTIONS requests cleanly
        // right here
        if (req->method() == drogon::Options) {
          auto resp = drogon::HttpResponse::newHttpResponse();
          resp->setStatusCode(drogon::k200OK);
          callback(resp);
          return;
        }

        std::string client_sdp = std::string(req->getBody());
        if (client_sdp.empty()) {
          auto bad_res = drogon::HttpResponse::newHttpResponse();
          bad_res->setStatusCode(drogon::k400BadRequest);
          bad_res->setBody("Missing SDP Offer");
          callback(bad_res);
          return;
        }

        rtc::Configuration config;
        config.iceServers.emplace_back("stun:stun.l.google.com:19302");

        g_peer_connection = std::make_shared<rtc::PeerConnection>(config);
        g_rtc_channel =
            g_peer_connection->createDataChannel("retro_mux_stream");

        g_rtc_channel->onMessage(
            [](rtc::binary message) {
              if (message.size() >= 3) {
                retro_mux_set_input(static_cast<int>(message[0]),
                                    static_cast<int>(message[1]),
                                    static_cast<bool>(message[2]));
              }
            },
            nullptr);

        g_rtc_channel->onOpen([]() {
          spdlog::info(
              "WebRTC Data Channel is officially OPEN. Ready for streaming!");
        });

        g_rtc_channel->onClosed(
            []() { spdlog::warn("WebRTC Data Channel closed."); });

        g_peer_connection->onLocalDescription(
            [cb = callback](rtc::Description description) {
              auto response = drogon::HttpResponse::newHttpResponse();
              response->setStatusCode(drogon::k200OK);
              response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
              response->setBody(std::string(description));

              cb(response);
              spdlog::info("Fully gathered SDP answer sent to client browser.");
            });

        try {
          // 1. Explicitly ingest the client string data as an incoming "Offer"
          // type
          rtc::Description remoteOffer(client_sdp,
                                       rtc::Description::Type::Offer);
          g_peer_connection->setRemoteDescription(remoteOffer);

          // 2. Explicitly force the local instance to generate an "Answer"
          // payload type
          g_peer_connection->setLocalDescription(
              rtc::Description::Type::Answer);

        } catch (const std::exception &e) {
          spdlog::error("WebRTC Handshake Engine parsing failure: {}",
                        e.what());
          auto error_res = drogon::HttpResponse::newHttpResponse();
          error_res->setStatusCode(drogon::k500InternalServerError);
          error_res->setBody("SDP Handshake Failed");
          callback(error_res);
        }
      },
      {drogon::Post, drogon::Options}); // 10. Launch network worker pool
  spdlog::info(
      "Starting Drogon signaling server on 0.0.0.0:8080... Let's play.");
  drogon::app().setThreadNum(4).addListener("0.0.0.0", 8080);

  // Keep the main thread alive safely inside Drogon's loop execution scope
  drogon::app().run();

  // Guard loop fallback block
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  if (retro_unload_game)
    retro_unload_game();
  if (retro_deinit)
    retro_deinit();
  dlclose(core_handle);

  spdlog::info("Run completed cleanly.");
  return 0;
}
