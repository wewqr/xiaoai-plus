#pragma once

#include <string>

namespace xiaoai_plus::config {

struct RealtimePreset {
  std::string ws_url{"wss://openspeech.bytedance.com/api/v3/realtime/dialogue"};
  std::string resource_id{"volc.speech.dialog"};
  std::string model{"1.2.1.0"};
  std::string bot_name{"豆包"};
  std::string system_role{"你是豆包同学，是小爱音箱上的语音助手。"};
  std::string speaking_style{"语气自然、友好、简洁。"};
  bool enable_music{true};
};

struct Realtime {
  std::string app_id;
  std::string access_token;
  std::string secret_key;
  RealtimePreset preset;
};

struct DeepSeek {
  std::string api_base{"https://api.deepseek.com"};
  std::string api_key;
  std::string model{"deepseek-flash"};
  std::string voice_id{"stella"};
  std::string system_role{
      "你是运行在小米智能音箱上的DeepSeek语音助手。回答自然、准确、简洁，适合直接朗读。"};
  int max_tokens{256};
  int vad_rms_threshold{500};
  int speech_end_ms{700};
  int max_utterance_seconds{30};
};

struct AudioPreset {
  std::string input_device{"noop"};
  std::string output_device{"default"};
  int sample_rate{16000};
  int channels{1};
  int bits_per_sample{16};
  int buffer_size{1440};
  int period_size{360};
  float playback_gain{1.0f};
};
using Audio = AudioPreset;

struct Wakeup {
  std::string say_hello{"在"};
  std::string keywords_file{"assets/keywords.txt"};
  std::string tokens_path{"assets/tokens.txt"};
  std::string encoder_path{"assets/encoder.onnx"};
  std::string decoder_path{"assets/decoder.onnx"};
  std::string joiner_path{"assets/joiner.onnx"};
};

struct BudgetPreset {
  int audio_chunk_ms{20};
  int input_queue_frames{64};
  int output_queue_frames{128};
  int reconnect_backoff_min_ms{300};
  int reconnect_backoff_max_ms{4000};
};

struct Config {
  Realtime realtime;
  DeepSeek deepseek;
  AudioPreset audio;
  Wakeup wakeup;
  BudgetPreset budget;

  void normalize();
  void validate() const;
};

Config load(const std::string& path);

}  // namespace xiaoai_plus::config
