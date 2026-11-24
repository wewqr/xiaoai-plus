#include "realtime/client.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <ixwebsocket/IXHttpClient.h>
#include <nlohmann/json.hpp>

#include "common/log.hpp"

namespace xiaoai_plus::realtime {

namespace {

// DeepSeek speech and chat run directly on the speaker through the official API.
const auto kLog = xiaoai_plus::GetLogger("deepseek");

void PutLe16(std::string* out, uint16_t value) {
  out->push_back(static_cast<char>(value & 0xff));
  out->push_back(static_cast<char>((value >> 8) & 0xff));
}

void PutLe32(std::string* out, uint32_t value) {
  out->push_back(static_cast<char>(value & 0xff));
  out->push_back(static_cast<char>((value >> 8) & 0xff));
  out->push_back(static_cast<char>((value >> 16) & 0xff));
  out->push_back(static_cast<char>((value >> 24) & 0xff));
}

std::string MakeWav(const std::vector<uint8_t>& pcm, int sample_rate) {
  std::string wav;
  wav.reserve(44 + pcm.size());
  wav.append("RIFF", 4);
  PutLe32(&wav, static_cast<uint32_t>(36 + pcm.size()));
  wav.append("WAVEfmt ", 8);
  PutLe32(&wav, 16);
  PutLe16(&wav, 1);
  PutLe16(&wav, 1);
  PutLe32(&wav, static_cast<uint32_t>(sample_rate));
  PutLe32(&wav, static_cast<uint32_t>(sample_rate * 2));
  PutLe16(&wav, 2);
  PutLe16(&wav, 16);
  wav.append("data", 4);
  PutLe32(&wav, static_cast<uint32_t>(pcm.size()));
  wav.append(reinterpret_cast<const char*>(pcm.data()), pcm.size());
  return wav;
}

double RmsS16(const std::vector<uint8_t>& pcm) {
  if (pcm.size() < 2) return 0.0;
  const auto* samples = reinterpret_cast<const int16_t*>(pcm.data());
  const size_t count = pcm.size() / sizeof(int16_t);
  long double sum = 0.0;
  for (size_t i = 0; i < count; ++i) {
    const long double value = samples[i];
    sum += value * value;
  }
  return std::sqrt(static_cast<double>(sum / count));
}

std::vector<uint8_t> Resample24kTo16k(const std::vector<uint8_t>& pcm24k) {
  const size_t input_count = pcm24k.size() / sizeof(int16_t);
  if (input_count < 2) return {};
  const auto* input = reinterpret_cast<const int16_t*>(pcm24k.data());
  const size_t output_count = input_count * 2 / 3;
  std::vector<uint8_t> output(output_count * sizeof(int16_t));
  auto* samples = reinterpret_cast<int16_t*>(output.data());
  for (size_t i = 0; i < output_count; ++i) {
    const double position = static_cast<double>(i) * 1.5;
    const size_t left = static_cast<size_t>(position);
    const size_t right = std::min(left + 1, input_count - 1);
    const double fraction = position - static_cast<double>(left);
    const double value = input[left] * (1.0 - fraction) + input[right] * fraction;
    samples[i] = static_cast<int16_t>(std::clamp(value, -32768.0, 32767.0));
  }
  return output;
}

std::vector<uint8_t> MakeWakeChime() {
  constexpr int kRate = 16000;
  constexpr int kFrames = 2880;  // 180 ms
  constexpr double kPi = 3.14159265358979323846;
  std::vector<uint8_t> pcm;
  pcm.reserve(kFrames * sizeof(int16_t));
  for (int i = 0; i < kFrames; ++i) {
    const double attack = std::min(1.0, i / 160.0);
    const double decay = std::min(1.0, (kFrames - i) / 640.0);
    const double t = static_cast<double>(i) / kRate;
    const double tone = std::sin(2.0 * kPi * 880.0 * t) +
                        0.35 * std::sin(2.0 * kPi * 1320.0 * t);
    const int16_t sample = static_cast<int16_t>(tone * 5000.0 * attack * decay);
    pcm.push_back(static_cast<uint8_t>(sample & 0xff));
    pcm.push_back(static_cast<uint8_t>((sample >> 8) & 0xff));
  }
  return pcm;
}

ix::HttpRequestArgsPtr MakeRequestArgs(const std::string& api_key,
                                       const std::string& content_type) {
  auto args = std::make_shared<ix::HttpRequestArgs>();
  args->connectTimeout = 15;
  args->transferTimeout = 120;
  args->compress = false;
  args->extraHeaders["Authorization"] = "Bearer " + api_key;
  args->extraHeaders["Content-Type"] = content_type;
  return args;
}

void ConfigureTls(ix::HttpClient* http) {
  ix::SocketTLSOptions tls;
  tls.caFile = "/etc/ssl/certs/ca-certificates.crt";
  http->setTLSOptions(tls);
}

bool HttpOk(const ix::HttpResponsePtr& response, const char* stage) {
  if (!response) {
    kLog->error("{} request returned no response", stage);
    return false;
  }
  if (response->errorCode != ix::HttpErrorCode::Ok || response->statusCode != 200) {
    kLog->error("{} request failed: http={}, transport={}", stage, response->statusCode,
                static_cast<int>(response->errorCode));
    return false;
  }
  return true;
}

}  // namespace

Client::Client(config::Config cfg, Callbacks callbacks)
    : cfg_(std::move(cfg)), callbacks_(std::move(callbacks)) {}

Client::~Client() { Stop(); }

bool Client::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) return true;
  // The one-character wake acknowledgement otherwise pays a fresh TLS + TTS
  // round trip on every wake (several seconds on the speaker).  Generate it
  // once at process startup and reuse the PCM locally.
  if (cfg_.wakeup.say_hello == "__chime__") {
    welcome_pcm16k_ = MakeWakeChime();
    kLog->info("local wake chime ready: bytes={}", welcome_pcm16k_.size());
  } else {
    std::vector<uint8_t> welcome_pcm24k;
    if (RequestTts(cfg_.wakeup.say_hello, &welcome_pcm24k)) {
      welcome_pcm16k_ = Resample24kTo16k(welcome_pcm24k);
      kLog->info("welcome audio cached: bytes={}", welcome_pcm16k_.size());
    } else {
      kLog->warn("welcome audio prefetch failed; will retry on wake");
    }
  }
  worker_thread_ = std::thread([this]() { WorkerLoop(); });
  return true;
}

void Client::Stop() {
  if (!running_.exchange(false)) return;
  session_active_.store(false);
  ready_for_speech_.store(false);
  queue_cv_.notify_all();
  if (worker_thread_.joinable()) worker_thread_.join();
}

bool Client::StartSession(std::chrono::milliseconds) {
  if (!running_.load()) return false;
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    audio_queue_.clear();
    welcome_pending_ = false;
  }
  history_.clear();
  ResetVad();
  session_active_.store(true);
  ready_for_speech_.store(false);
  kLog->info("DeepSeek session started");
  return true;
}

bool Client::FinishSession(std::chrono::milliseconds) {
  session_active_.store(false);
  ready_for_speech_.store(false);
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    audio_queue_.clear();
    welcome_pending_ = false;
  }
  ResetVad();
  kLog->info("DeepSeek session finished");
  return true;
}

bool Client::EnqueueAudio(const std::vector<uint8_t>& chunk) {
  if (!session_active_.load() || !ready_for_speech_.load() || chunk.empty()) return true;
  std::lock_guard<std::mutex> lock(queue_mu_);
  if (audio_queue_.size() >= static_cast<size_t>(cfg_.budget.input_queue_frames)) {
    audio_queue_.pop_front();
  }
  audio_queue_.push_back(chunk);
  queue_cv_.notify_one();
  return true;
}

bool Client::EnqueueAudio(std::vector<uint8_t>&& chunk) {
  if (!session_active_.load() || !ready_for_speech_.load() || chunk.empty()) return true;
  std::lock_guard<std::mutex> lock(queue_mu_);
  if (audio_queue_.size() >= static_cast<size_t>(cfg_.budget.input_queue_frames)) {
    audio_queue_.pop_front();
  }
  audio_queue_.push_back(std::move(chunk));
  queue_cv_.notify_one();
  return true;
}

bool Client::SendSayHello() {
  if (!session_active_.load()) return false;
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    welcome_pending_ = true;
  }
  queue_cv_.notify_one();
  return true;
}

void Client::WorkerLoop() {
  while (running_.load()) {
    std::vector<uint8_t> chunk;
    bool welcome = false;
    {
      std::unique_lock<std::mutex> lock(queue_mu_);
      queue_cv_.wait(lock, [this]() {
        return !running_.load() || welcome_pending_ || !audio_queue_.empty();
      });
      if (!running_.load()) break;
      if (welcome_pending_) {
        welcome_pending_ = false;
        welcome = true;
        audio_queue_.clear();
      } else {
        chunk = std::move(audio_queue_.front());
        audio_queue_.pop_front();
      }
    }

    if (welcome) {
      if (!Speak(cfg_.wakeup.say_hello)) {
        CloseWithError("deepseek_tts_error");
        continue;
      }
      ResetVad();
      ready_for_speech_.store(session_active_.load());
      continue;
    }
    if (!chunk.empty()) HandleAudioChunk(chunk);
  }
}

void Client::HandleAudioChunk(const std::vector<uint8_t>& chunk) {
  if (!session_active_.load() || !ready_for_speech_.load() || processing_.load()) return;
  const int frame_ms = std::max(1, static_cast<int>(
      chunk.size() * 1000 / (sizeof(int16_t) * cfg_.audio.sample_rate)));
  const double rms = RmsS16(chunk);
  const double threshold = std::max<double>(cfg_.deepseek.vad_rms_threshold, noise_rms_ * 3.2);
  const bool voice = rms >= threshold;

  if (!speech_started_) {
    pre_roll_.push_back(chunk);
    pre_roll_ms_ += frame_ms;
    while (pre_roll_ms_ > 400 && !pre_roll_.empty()) {
      pre_roll_ms_ -= std::max(1, static_cast<int>(pre_roll_.front().size() * 1000 /
          (sizeof(int16_t) * cfg_.audio.sample_rate)));
      pre_roll_.pop_front();
    }
    if (!voice) {
      noise_rms_ = noise_rms_ * 0.97 + rms * 0.03;
      voice_run_ms_ = 0;
      return;
    }
    voice_run_ms_ += frame_ms;
    if (voice_run_ms_ < 90) return;

    speech_started_ = true;
    for (const auto& frame : pre_roll_) {
      utterance_.insert(utterance_.end(), frame.begin(), frame.end());
    }
    pre_roll_.clear();
    pre_roll_ms_ = 0;
    speech_ms_ = voice_run_ms_;
    silence_ms_ = 0;
    if (callbacks_.on_user_activity) callbacks_.on_user_activity();
    kLog->info("speech started");
    return;
  }

  utterance_.insert(utterance_.end(), chunk.begin(), chunk.end());
  if (voice) {
    speech_ms_ += frame_ms;
    silence_ms_ = 0;
  } else {
    silence_ms_ += frame_ms;
  }

  const int utterance_ms = static_cast<int>(
      utterance_.size() * 1000 / (sizeof(int16_t) * cfg_.audio.sample_rate));
  const bool reached_end = speech_ms_ >= 250 && silence_ms_ >= cfg_.deepseek.speech_end_ms;
  const bool reached_limit = utterance_ms >= cfg_.deepseek.max_utterance_seconds * 1000;
  if (!reached_end && !reached_limit) return;

  std::vector<uint8_t> utterance = std::move(utterance_);
  ResetVad();
  ready_for_speech_.store(false);
  processing_.store(true);
  const bool ok = ProcessUtterance(utterance);
  processing_.store(false);
  if (!ok) {
    CloseWithError("deepseek_api_error");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    audio_queue_.clear();
  }
  ready_for_speech_.store(session_active_.load());
}

void Client::ResetVad() {
  pre_roll_.clear();
  utterance_.clear();
  speech_started_ = false;
  voice_run_ms_ = 0;
  silence_ms_ = 0;
  speech_ms_ = 0;
  pre_roll_ms_ = 0;
  noise_rms_ = 120.0;
}

bool Client::ProcessUtterance(const std::vector<uint8_t>& pcm) {
  std::string user_text;
  if (!RequestAsr(pcm, &user_text)) return false;
  if (user_text.empty()) {
    kLog->info("ASR returned empty text");
    return true;
  }
  kLog->info("ASR completed: chars={}", user_text.size());
  // The stock XiaoAI service listens independently.  When an active
  // DeepSeek conversation hears its wake phrase, yield immediately and do
  // not send the utterance to DeepSeek.
  if (user_text.find("小爱同学") != std::string::npos) {
    kLog->info("XiaoAI handoff detected; closing DeepSeek session");
    session_active_.store(false);
    ready_for_speech_.store(false);
    if (callbacks_.on_session_closed) callbacks_.on_session_closed("xiaoai_handoff");
    return true;
  }
  if (callbacks_.on_asr_final) callbacks_.on_asr_final(user_text);

  std::string reply;
  if (!RequestChat(user_text, &reply) || reply.empty()) return false;
  kLog->info("chat completed: chars={}", reply.size());
  history_.emplace_back(user_text, reply);
  while (history_.size() > 6) history_.pop_front();
  const bool spoken = Speak(reply);
  if (callbacks_.on_chat_ended) callbacks_.on_chat_ended();
  return spoken;
}

bool Client::RequestAsr(const std::vector<uint8_t>& pcm, std::string* text) {
  if (!text) return false;
  ix::HttpClient http;
  ConfigureTls(&http);
  const std::string boundary = "----xiaoai-deepseek-audio";
  const std::string wav = MakeWav(pcm, cfg_.audio.sample_rate);
  std::string body;
  body.reserve(wav.size() + 256);
  body += "--" + boundary + "\r\n";
  body += "Content-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n";
  body += "Content-Type: audio/wav\r\n\r\n";
  body.append(wav);
  body += "\r\n--" + boundary + "--\r\n";
  auto args = MakeRequestArgs(cfg_.deepseek.api_key, "multipart/form-data; boundary=" + boundary);
  args->extraHeaders["Accept"] = "application/json";
  auto response = http.post(cfg_.deepseek.api_base + "/v1/tools/asr", body, args);
  if (!HttpOk(response, "asr")) return false;
  try {
    auto json = nlohmann::json::parse(response->body);
    *text = json.value("text", "");
    return true;
  } catch (...) {
    kLog->error("asr response was not valid JSON");
    return false;
  }
}

bool Client::RequestChat(const std::string& user_text, std::string* reply) {
  if (!reply) return false;
  nlohmann::json messages = nlohmann::json::array();
  messages.push_back({{"role", "system"}, {"content", cfg_.deepseek.system_role}});
  for (const auto& turn : history_) {
    messages.push_back({{"role", "user"}, {"content", turn.first}});
    messages.push_back({{"role", "assistant"}, {"content", turn.second}});
  }
  messages.push_back({{"role", "user"}, {"content", user_text}});
  nlohmann::json payload = {
      {"model", cfg_.deepseek.model},
      {"messages", std::move(messages)},
      {"thinking", {{"type", "disabled"}}},
      {"max_tokens", cfg_.deepseek.max_tokens},
      {"stream", false},
  };
  ix::HttpClient http;
  ConfigureTls(&http);
  auto args = MakeRequestArgs(cfg_.deepseek.api_key, "application/json");
  args->extraHeaders["Accept"] = "application/json";
  auto response = http.post(cfg_.deepseek.api_base + "/chat/completions", payload.dump(), args);
  if (!HttpOk(response, "chat")) return false;
  try {
    auto json = nlohmann::json::parse(response->body);
    if (!json.contains("choices") || json["choices"].empty()) return false;
    *reply = json["choices"][0]["message"].value("content", "");
    return !reply->empty();
  } catch (...) {
    kLog->error("chat response was not valid JSON");
    return false;
  }
}

bool Client::RequestTts(const std::string& text, std::vector<uint8_t>* pcm24k) {
  if (!pcm24k) return false;
  nlohmann::json payload = {
      {"text", text},
      {"voice_id", cfg_.deepseek.voice_id},
      {"format", "pcm"},
      {"filter_markdown", true},
  };
  ix::HttpClient http;
  ConfigureTls(&http);
  auto args = MakeRequestArgs(cfg_.deepseek.api_key, "application/json");
  args->extraHeaders["Accept"] = "application/octet-stream";
  auto response = http.post(cfg_.deepseek.api_base + "/v1/tools/tts", payload.dump(), args);
  if (!HttpOk(response, "tts")) return false;
  pcm24k->assign(response->body.begin(), response->body.end());
  return !pcm24k->empty();
}

bool Client::Speak(const std::string& text) {
  if (!session_active_.load()) return false;
  if (callbacks_.on_set_ai_speaking) callbacks_.on_set_ai_speaking(true);
  std::vector<uint8_t> pcm16k;
  if (text == cfg_.wakeup.say_hello && !welcome_pcm16k_.empty()) {
    pcm16k = welcome_pcm16k_;
  } else {
    std::vector<uint8_t> pcm24k;
    if (!RequestTts(text, &pcm24k)) {
      if (callbacks_.on_set_ai_speaking) callbacks_.on_set_ai_speaking(false);
      return false;
    }
    pcm16k = Resample24kTo16k(pcm24k);
    if (text == cfg_.wakeup.say_hello && !pcm16k.empty()) {
      welcome_pcm16k_ = pcm16k;
    }
  }
  constexpr size_t kChunkBytes = 640;
  for (size_t offset = 0; offset < pcm16k.size() && session_active_.load();
       offset += kChunkBytes) {
    const size_t end = std::min(offset + kChunkBytes, pcm16k.size());
    std::vector<uint8_t> chunk(pcm16k.begin() + offset, pcm16k.begin() + end);
    if (callbacks_.on_audio) callbacks_.on_audio(chunk);
    // Player's queue is intentionally small.  Pace the raw PCM at real-time
    // speed so long answers are not queued all at once and silently dropped.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (callbacks_.on_set_ai_speaking) callbacks_.on_set_ai_speaking(false);
  return session_active_.load();
}

void Client::CloseWithError(const std::string& reason) {
  session_active_.store(false);
  ready_for_speech_.store(false);
  kLog->error("closing DeepSeek session: {}", reason);
  if (callbacks_.on_session_closed) callbacks_.on_session_closed(reason);
}

}  // namespace xiaoai_plus::realtime
