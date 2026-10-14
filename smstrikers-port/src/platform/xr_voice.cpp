// strikers-quest r44: see xr_voice.h.
//
// One thread of its own opens and closes the two AAudio streams (opening one takes tens of
// milliseconds, never on the game thread) and reads the microphone in 20 ms frames: Opus, a level gate
// (frames that are only room noise are coded but not sent, so a quiet player costs nothing), and a
// queue that xr_net.cpp empties into each input message. The friend's frames go into a short queue
// that the speaker stream's callback decodes from, after 60 ms of them have gathered (the input
// messages arrive in clumps of two or three frames, and a stall delivers several at once).
#include "xr_voice.h"

#include <SDL3/SDL_system.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <jni.h>
#include <opus.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace xrvoice {
namespace {

// Into the game log too, like the network's lines (xr_net.cpp NLOG).
void voice_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void voice_log(const char* fmt, ...) {
  char line[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  __android_log_print(ANDROID_LOG_INFO, "strikers_xr", "%s", line);
  std::fprintf(stderr, "[xr] %s\n", line);
}
#define VLOG(...) voice_log(__VA_ARGS__)

constexpr int kRate = 48000;
constexpr int kFrame = kRate / 50;    // 20 ms
constexpr size_t kMaxOut = 25;        // 0.5 s waiting to go at most (a stalled link): older is dropped
constexpr size_t kMaxIn = 12;         // 240 ms queued to play at most: older is dropped
constexpr size_t kStartDepth = 3;     // 60 ms gathered before the speaker starts (again)
constexpr uint64_t kStartWaitMs = 90; // or this long after the first one came, whichever is first
constexpr int kHangFrames = 15;       // the gate stays open 300 ms after the voice drops
constexpr float kGateRms = 0.012f;    // about -38 dBFS: speech at a headset's mic, not the room

uint64_t now_ms() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

struct Packet {
  uint16_t seq = 0;
  uint8_t len = 0;
  uint8_t data[255];
};

std::mutex m;               // everything below
bool loaded = false;        // voice.txt read
bool on = false;            // the switch
bool inMatch = false;
bool loopTest = false;      // STRIKERS_VOICE_LOOP=1: your own voice back to you, no match needed
std::deque<Packet> outQ;    // to the friend
std::deque<Packet> inQ;     // from the friend
uint64_t inFirstMs = 0;     // when inQ last went from empty to not
bool playing = false;       // the speaker is draining inQ (else gathering)
uint32_t statSent = 0, statHeard = 0, statUnderruns = 0, statDropped = 0;

std::atomic<bool> threadUp{false};
std::atomic<int> micAllowed{-1};  // -1 not asked yet
std::atomic<bool> outDead{false}, inDead{false};

std::string voice_path() {
  const char* home = std::getenv("STRIKERS_HOME");
  return std::string(home != nullptr && *home != '\0' ? home : "/storage/emulated/0/Strikers") + "/voice.txt";
}

void load_locked() {
  int v = 0;
  if (FILE* f = std::fopen(voice_path().c_str(), "r")) {
    if (std::fscanf(f, "%d", &v) != 1) v = 0;
    std::fclose(f);
  }
  on = v != 0;
  const char* loop = std::getenv("STRIKERS_VOICE_LOOP");
  loopTest = loop != nullptr && loop[0] == '1';
  loaded = true;
}

// Android's own answer, through the activity (SDL attaches this thread to Java when it has to).
bool ask_permission_state() {
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) return false;
  bool granted = false;
  jclass cls = env->GetObjectClass(activity);
  jmethodID check = cls != nullptr ? env->GetMethodID(cls, "checkSelfPermission", "(Ljava/lang/String;)I") : nullptr;
  jstring perm = env->NewStringUTF("android.permission.RECORD_AUDIO");
  if (check != nullptr && perm != nullptr && !env->ExceptionCheck()) {
    granted = env->CallIntMethod(activity, check, perm) == 0;  // PackageManager.PERMISSION_GRANTED
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    granted = false;
  }
  if (perm != nullptr) env->DeleteLocalRef(perm);
  if (cls != nullptr) env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
  return granted;
}

// ---------------------------------------------------------------------------------------------
// The speaker: the friend's voice, decoded in the stream's own callback
// ---------------------------------------------------------------------------------------------

struct Playback {
  OpusDecoder* dec = nullptr;
  float pcm[kFrame * 2];
  int count = 0, at = 0;
  int rate = kRate;
  int concealed = 0;  // frames made up in a row (the callback's own)
};
Playback play;

aaudio_data_callback_result_t on_speaker(AAudioStream*, void*, void* audio, int32_t frames) {
  auto* outBuf = static_cast<float*>(audio);
  int32_t done = 0;
  while (done < frames) {
    if (play.at < play.count) {
      const int n = std::min<int>(frames - done, play.count - play.at);
      for (int i = 0; i < n; ++i) outBuf[done + i] = std::clamp(play.pcm[play.at + i] * 1.4f, -1.0f, 1.0f);
      play.at += n;
      done += n;
      continue;
    }
    Packet p;
    bool got = false, conceal = false;
    {
      std::lock_guard<std::mutex> lock(m);
      if (!playing && (inQ.size() >= kStartDepth || (!inQ.empty() && now_ms() - inFirstMs >= kStartWaitMs))) {
        playing = true;
        play.concealed = 0;
      }
      if (playing) {
        if (!inQ.empty()) {
          p = inQ.front();
          inQ.pop_front();
          got = true;
          play.concealed = 0;
        } else if (play.concealed < 3) {
          // Ran dry mid-word (a clump late): Opus fills up to 60 ms in, so a late clump carries on
          // where it should instead of after a gap of gathering again.
          conceal = true;
          ++play.concealed;
        } else {
          playing = false;  // the end of a sentence (or a long hold-up): gather again
          ++statUnderruns;
        }
      }
    }
    if (!got && !conceal) {
      std::memset(outBuf + done, 0, sizeof(float) * static_cast<size_t>(frames - done));
      break;
    }
    const int n = conceal ? opus_decode_float(play.dec, nullptr, 0, play.pcm, play.rate / 50, 0)
                          : opus_decode_float(play.dec, p.data, p.len, play.pcm, play.rate / 50, 0);
    play.count = n > 0 ? n : 0;
    play.at = 0;
  }
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void on_stream_error(AAudioStream*, void* which, aaudio_result_t error) {
  // Not closed here (AAudio forbids it in the callback): the voice thread reopens it.
  (which == &play ? outDead : inDead) = true;
  VLOG("voice: %s stream error %s", which == &play ? "speaker" : "microphone", AAudio_convertResultToText(error));
}

AAudioStream* open_speaker() {
  AAudioStreamBuilder* b = nullptr;
  if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return nullptr;
  AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
  AAudioStreamBuilder_setSampleRate(b, kRate);
  AAudioStreamBuilder_setChannelCount(b, 1);
  AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
  // The game's own volume (media), not a phone call's: the Quest's volume buttons work on it.
  AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
  AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SPEECH);
  AAudioStreamBuilder_setDataCallback(b, on_speaker, nullptr);
  AAudioStreamBuilder_setErrorCallback(b, on_stream_error, &play);
  AAudioStream* s = nullptr;
  aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
  AAudioStreamBuilder_delete(b);
  if (r != AAUDIO_OK) {
    VLOG("voice: no speaker stream (%s)", AAudio_convertResultToText(r));
    return nullptr;
  }
  play.rate = AAudioStream_getSampleRate(s);
  int err = 0;
  if (play.dec != nullptr) opus_decoder_destroy(play.dec);
  play.dec = opus_decoder_create(play.rate, 1, &err);
  if (play.dec == nullptr || AAudioStream_getChannelCount(s) != 1 || play.rate / 50 > kFrame * 2) {
    VLOG("voice: speaker stream at %d Hz x%d is not one Opus can feed", play.rate, AAudioStream_getChannelCount(s));
    AAudioStream_close(s);
    return nullptr;
  }
  play.count = play.at = 0;
  outDead = false;
  if ((r = AAudioStream_requestStart(s)) != AAUDIO_OK) {
    VLOG("voice: the speaker stream did not start (%s)", AAudio_convertResultToText(r));
    AAudioStream_close(s);
    return nullptr;
  }
  return s;
}

// ---------------------------------------------------------------------------------------------
// The microphone
// ---------------------------------------------------------------------------------------------

struct Capture {
  AAudioStream* stream = nullptr;
  OpusEncoder* enc = nullptr;
  int rate = kRate, channels = 1, frame = kFrame;
  int16_t raw[kFrame * 2];   // as read (up to two channels)
  int16_t pcm[kFrame];       // one channel, one 20 ms frame
  int have = 0;
  uint16_t seq = 0;
  int hang = 0;
  bool hasPrev = false;      // the frame before the gate opened, sent with it (the word's start)
  Packet prev;
  uint64_t retryAt = 0;
};
Capture cap;

void close_mic() {
  if (cap.stream != nullptr) {
    AAudioStream_requestStop(cap.stream);
    AAudioStream_close(cap.stream);
    cap.stream = nullptr;
  }
  if (cap.enc != nullptr) {
    opus_encoder_destroy(cap.enc);
    cap.enc = nullptr;
  }
}

bool open_mic() {
  AAudioStreamBuilder* b = nullptr;
  if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
  AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
  AAudioStreamBuilder_setSampleRate(b, kRate);
  AAudioStreamBuilder_setChannelCount(b, 1);
  AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
  // The voice-call path: the system's echo canceller and noise suppressor, so the game's sound and
  // the friend's voice out of the headset's speakers stay out of what the friend hears.
  AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
  AAudioStreamBuilder_setErrorCallback(b, on_stream_error, &cap);
  aaudio_result_t r = AAudioStreamBuilder_openStream(b, &cap.stream);
  AAudioStreamBuilder_delete(b);
  if (r != AAUDIO_OK) {
    VLOG("voice: no microphone stream (%s)", AAudio_convertResultToText(r));
    cap.stream = nullptr;
    return false;
  }
  cap.rate = AAudioStream_getSampleRate(cap.stream);
  cap.channels = AAudioStream_getChannelCount(cap.stream);
  cap.frame = cap.rate / 50;
  int err = 0;
  const bool opusRate = cap.rate == 8000 || cap.rate == 12000 || cap.rate == 16000 || cap.rate == 24000 ||
                        cap.rate == 48000;
  cap.enc = opusRate && cap.channels >= 1 && cap.channels <= 2
                ? opus_encoder_create(cap.rate, 1, OPUS_APPLICATION_VOIP, &err)
                : nullptr;
  if (cap.enc == nullptr) {
    VLOG("voice: microphone at %d Hz x%d is not one Opus takes", cap.rate, cap.channels);
    close_mic();
    return false;
  }
  opus_encoder_ctl(cap.enc, OPUS_SET_BITRATE(20000));
  opus_encoder_ctl(cap.enc, OPUS_SET_COMPLEXITY(5));
  opus_encoder_ctl(cap.enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  // The room server's link is TCP: nothing is lost on the way, so no in-band redundancy.
  opus_encoder_ctl(cap.enc, OPUS_SET_INBAND_FEC(0));
  cap.have = 0;
  cap.hang = 0;
  cap.hasPrev = false;
  inDead = false;
  if ((r = AAudioStream_requestStart(cap.stream)) != AAUDIO_OK) {
    VLOG("voice: the microphone did not start (%s)", AAudio_convertResultToText(r));
    close_mic();
    return false;
  }
  VLOG("voice: microphone on (%d Hz)", cap.rate);
  return true;
}

void queue_out(const Packet& p, bool loop) {
  std::lock_guard<std::mutex> lock(m);
  std::deque<Packet>& q = loop ? inQ : outQ;
  if (loop && q.empty()) inFirstMs = now_ms();
  q.push_back(p);
  if (q.size() > (loop ? kMaxIn : kMaxOut)) {
    q.pop_front();
    ++statDropped;
  }
}

// One 20 ms frame in cap.pcm: coded, and sent if the gate is open.
void process_frame(bool loop) {
  double sum = 0.0;
  for (int i = 0; i < cap.frame; ++i) sum += static_cast<double>(cap.pcm[i]) * cap.pcm[i];
  const float rms = static_cast<float>(std::sqrt(sum / cap.frame) / 32768.0);
  Packet p;
  p.seq = cap.seq++;
  const opus_int32 n = opus_encode(cap.enc, cap.pcm, cap.frame, p.data, sizeof(p.data));
  if (n <= 0) return;
  p.len = static_cast<uint8_t>(n);
  if (rms >= kGateRms) {
    if (cap.hang == 0 && cap.hasPrev) queue_out(cap.prev, loop);
    cap.hang = kHangFrames;
  }
  if (cap.hang > 0) {
    --cap.hang;
    queue_out(p, loop);
    cap.hasPrev = false;
  } else {
    cap.prev = p;
    cap.hasPrev = true;
  }
}

// Whatever the microphone has, up to one frame, waiting at most 40 ms for it.
void read_mic(bool loop) {
  const int want = cap.frame - cap.have;
  const aaudio_result_t got = AAudioStream_read(cap.stream, cap.raw, want, 40'000'000);
  if (got < 0) {
    VLOG("voice: microphone read failed (%s)", AAudio_convertResultToText(got));
    inDead = true;
    return;
  }
  for (int i = 0; i < got; ++i) {
    cap.pcm[cap.have + i] = cap.channels == 2
                                ? static_cast<int16_t>((static_cast<int>(cap.raw[2 * i]) + cap.raw[2 * i + 1]) / 2)
                                : cap.raw[i];
  }
  cap.have += got;
  if (cap.have == cap.frame) {
    cap.have = 0;
    process_frame(loop);
  }
}

void voice_thread() {
  AAudioStream* speaker = nullptr;
  uint64_t lastStatsMs = now_ms();
  bool noMicLogged = false;
  for (;;) {
    bool want = false, loop = false;
    {
      std::lock_guard<std::mutex> lock(m);
      loop = loopTest;
      want = on && (inMatch || loopTest);
    }
    if (!want) {
      if (speaker != nullptr || cap.stream != nullptr) {
        close_mic();
        if (speaker != nullptr) {
          AAudioStream_requestStop(speaker);
          AAudioStream_close(speaker);
          speaker = nullptr;
        }
        std::lock_guard<std::mutex> lock(m);
        VLOG("voice: off (%u frames sent, %u heard, %u gaps, %u dropped)", statSent, statHeard, statUnderruns,
             statDropped);
        outQ.clear();
        inQ.clear();
        playing = false;
      }
      usleep(50000);
      continue;
    }
    if (outDead.load() && speaker != nullptr) {
      AAudioStream_close(speaker);
      speaker = nullptr;
    }
    if (inDead.load() && cap.stream != nullptr) {
      close_mic();
      cap.retryAt = now_ms() + 2000;
    }
    if (speaker == nullptr) {
      speaker = open_speaker();
      if (speaker == nullptr) {
        usleep(500000);
        continue;
      }
      VLOG("voice: speaker on (%d Hz)", play.rate);
    }
    if (cap.stream == nullptr && now_ms() >= cap.retryAt) {
      if (micAllowed.load() != 1) micAllowed = ask_permission_state() ? 1 : 0;
      if (micAllowed.load() == 1) {
        if (!open_mic()) cap.retryAt = now_ms() + 5000;
        noMicLogged = false;
      } else {
        cap.retryAt = now_ms() + 3000;  // looked at again: the panel may grant it meanwhile
        if (!noMicLogged) VLOG("voice: no microphone permission; you hear your friend, they don't hear you");
        noMicLogged = true;
      }
    }
    if (cap.stream != nullptr) {
      read_mic(loop);
    } else {
      usleep(20000);
    }
    if (now_ms() - lastStatsMs > 10000) {
      lastStatsMs = now_ms();
      std::lock_guard<std::mutex> lock(m);
      VLOG("voice: %u frames sent, %u heard, %u gaps, %u dropped, %zu waiting to play", statSent, statHeard,
           statUnderruns, statDropped, inQ.size());
    }
  }
}

void ensure_thread() {
  bool expected = false;
  if (threadUp.compare_exchange_strong(expected, true)) std::thread(voice_thread).detach();
}

void permission_answer(void*, const char*, bool granted) {
  micAllowed = granted ? 1 : 0;
  VLOG("voice: microphone %s", granted ? "allowed" : "refused");
}

}  // namespace

bool enabled() {
  std::lock_guard<std::mutex> lock(m);
  if (!loaded) load_locked();
  return on;
}

void set_enabled(bool v) {
  bool loop = false, match = false;
  {
    std::lock_guard<std::mutex> lock(m);
    if (!loaded) load_locked();
    on = v;
    loop = loopTest;
    match = inMatch;
    if (FILE* f = std::fopen(voice_path().c_str(), "w")) {
      std::fprintf(f, "%d\n", v ? 1 : 0);
      std::fclose(f);
    }
  }
  VLOG("voice chat %s", v ? "on" : "off");
  // Asked here only between matches: Android's question takes the headset's focus, and a match's
  // lockstep would stall behind it. The panel asks when it is turned on there.
  if (v && !match && micAllowed.load() != 1) {
    micAllowed = -1;
    SDL_RequestAndroidPermission("android.permission.RECORD_AUDIO", permission_answer, nullptr);
  }
  if (v && (match || loop)) ensure_thread();
}

bool mic_allowed() {
  if (micAllowed.load() < 0) micAllowed = ask_permission_state() ? 1 : 0;
  return micAllowed.load() == 1;
}

void match_begin() {
  bool want = false;
  {
    std::lock_guard<std::mutex> lock(m);
    load_locked();  // the panel may have flipped it since
    inMatch = true;
    outQ.clear();
    inQ.clear();
    playing = false;
    statSent = statHeard = statUnderruns = statDropped = 0;
    want = on;
  }
  micAllowed = -1;  // asked again by the voice thread
  if (want) ensure_thread();
  VLOG("voice: match started, voice chat %s", want ? "on" : "off");
}

void match_end() {
  std::lock_guard<std::mutex> lock(m);
  inMatch = false;
  outQ.clear();
  inQ.clear();
  playing = false;
}

void append_outgoing(std::vector<uint8_t>& msg, size_t maxTotal) {
  const size_t countAt = msg.size();
  msg.push_back(0);
  std::lock_guard<std::mutex> lock(m);
  size_t room = maxTotal > msg.size() ? maxTotal - msg.size() : 0;
  uint8_t n = 0;
  while (!outQ.empty() && n < 16) {
    const Packet& p = outQ.front();
    const size_t need = 3u + p.len;
    if (need > room) break;
    msg.push_back(static_cast<uint8_t>(p.seq));
    msg.push_back(static_cast<uint8_t>(p.seq >> 8));
    msg.push_back(p.len);
    msg.insert(msg.end(), p.data, p.data + p.len);
    room -= need;
    ++n;
    ++statSent;
    outQ.pop_front();
  }
  msg[countAt] = n;
}

void take_incoming(const uint8_t* data, size_t size) {
  if (size == 0) return;
  std::lock_guard<std::mutex> lock(m);
  if (!on || !inMatch) return;  // the switch off: neither way
  const uint8_t n = data[0];
  size_t at = 1;
  for (uint8_t i = 0; i < n; ++i) {
    if (at + 3 > size) return;
    Packet p;
    p.seq = static_cast<uint16_t>(data[at] | (data[at + 1] << 8));
    p.len = data[at + 2];
    at += 3;
    if (p.len == 0 || at + p.len > size) return;
    std::memcpy(p.data, data + at, p.len);
    at += p.len;
    if (inQ.empty()) inFirstMs = now_ms();
    inQ.push_back(p);
    ++statHeard;
    if (inQ.size() > kMaxIn) {
      inQ.pop_front();
      ++statDropped;
    }
  }
}

}  // namespace xrvoice
