#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "config/config.hpp"

namespace xiaoai_plus::realtime {

class Client {
 public:
  struct Callbacks {
    std::function<void(const std::vector<uint8_t>&)> on_audio;
    std::function<void(bool)> on_set_ai_speaking;
    std::function<void(const std::string&)> on_asr_final;
    std::function<void()> on_user_activity;
    std::function<void(const std::string&)> on_session_closed;
    std::function<void()> on_chat_ended;
  };

  explicit Client(config::Config cfg, Callbacks callbacks);
  ~Client();

  bool Start();
  void Stop();
  bool StartSession(std::chrono::milliseconds timeout = std::chrono::seconds(10));
  bool FinishSession(std::chrono::milliseconds timeout = std::chrono::seconds(2));
  void CancelForStockHandoff();
  bool EnqueueAudio(const std::vector<uint8_t>& chunk);
  bool EnqueueAudio(std::vector<uint8_t>&& chunk);
  bool SendSayHello();

 private:
  void WorkerLoop();
  void HandleAudioChunk(const std::vector<uint8_t>& chunk);
  void ResetVad();
  bool ProcessUtterance(const std::vector<uint8_t>& pcm);
  bool RequestAsr(const std::vector<uint8_t>& pcm, std::string* text);
  bool RequestChat(const std::string& user_text, std::string* reply);
  bool RequestTts(const std::string& text, std::vector<uint8_t>* pcm24k);
  bool Speak(const std::string& text);
  void CloseWithError(const std::string& reason);

  config::Config cfg_;
  Callbacks callbacks_;
  std::atomic<bool> running_{false};
  std::atomic<bool> session_active_{false};
  std::atomic<bool> ready_for_speech_{false};
  std::atomic<bool> processing_{false};

  std::mutex queue_mu_;
  std::condition_variable queue_cv_;
  std::deque<std::vector<uint8_t>> audio_queue_;
  bool welcome_pending_{false};
  std::thread worker_thread_;
  std::vector<uint8_t> welcome_pcm16k_;

  std::deque<std::vector<uint8_t>> pre_roll_;
  std::vector<uint8_t> utterance_;
  bool speech_started_{false};
  int voice_run_ms_{0};
  int silence_ms_{0};
  int speech_ms_{0};
  int pre_roll_ms_{0};
  double noise_rms_{120.0};

  std::deque<std::pair<std::string, std::string>> history_;
};

}  // namespace xiaoai_plus::realtime
