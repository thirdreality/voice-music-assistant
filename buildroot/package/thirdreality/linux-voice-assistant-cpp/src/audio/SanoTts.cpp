#include "audio/SanoTts.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "espeak-ng/speak_lib.h"
#include "snt_cp_id_tables.h"
#include "snt_front_f32.h"
#include "snt_piperlite.h"
#include "util/Log.h"

namespace lva::audio {

namespace {

constexpr const char* kTag = "SanoTts";

constexpr int kSampleRate = 22050;
constexpr int kHop        = SNT_PIPERLITE_HOP;

// Decoder chunking: 32 latent frames (~0.37 s) per pass plus 8 frames of
// context on each side. 8 frames keeps the seams bit-identical in practice
// (SNR > 60 dB vs one full pass); fewer frames gives audible clicks.
constexpr int kChunkFrames   = 32;
constexpr int kOverlapFrames = 8;

// Piper framing ids.
constexpr std::int32_t kIdPad = 0;
constexpr std::int32_t kIdBos = 1;
constexpr std::int32_t kIdEos = 2;

// Schwa: stands in for ids outside a student's vocabulary (upstream rule).
constexpr std::int32_t kIdFallback = 59;

// Upstream renders every English voice at 1.08.
constexpr float kBaseLengthScale = 1.08f;

// eSpeak voice the amy model was trained with (Piper config).
constexpr const char* kEspeakVoice = "en-us";

constexpr int kIdTableSize =
    static_cast<int>(sizeof(SNT_CP_ID_AMY) / sizeof(SNT_CP_ID_AMY[0]));

// IPA codepoint -> Piper phoneme id, or -1.
int LookupId(unsigned cp) {
    int lo = 0, hi = kIdTableSize;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (SNT_CP_ID_AMY[mid].cp < cp) lo = mid + 1;
        else hi = mid;
    }
    return (lo < kIdTableSize && SNT_CP_ID_AMY[lo].cp == cp)
               ? SNT_CP_ID_AMY[lo].id
               : -1;
}

std::int32_t ReadI32(const unsigned char* p) {
    return static_cast<std::int32_t>(
        static_cast<std::uint32_t>(p[0]) |
        (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) |
        (static_cast<std::uint32_t>(p[3]) << 24));
}

const char* NextCodepoint(const char* s, unsigned* cp) {
    const auto c = static_cast<unsigned char>(*s);
    if (c < 0x80) { *cp = c; return s + 1; }
    if ((c >> 5) == 6 && s[1]) {
        *cp = ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu);
        return s + 2;
    }
    if ((c >> 4) == 14 && s[1] && s[2]) {
        *cp = ((c & 0xFu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
        return s + 3;
    }
    if ((c >> 3) == 30 && s[1] && s[2] && s[3]) {
        *cp = ((c & 7u) << 18) | ((s[1] & 0x3Fu) << 12) |
              ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
        return s + 4;
    }
    *cp = c;
    return s + 1;
}

// Read-only file mapping.
class MappedFile {
   public:
    MappedFile() = default;
    ~MappedFile() { Reset(); }
    MappedFile(const MappedFile&)            = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& o) noexcept
        : data_(std::exchange(o.data_, nullptr)),
          size_(std::exchange(o.size_, 0)) {}
    MappedFile& operator=(MappedFile&& o) noexcept {
        if (this != &o) {
            Reset();
            data_ = std::exchange(o.data_, nullptr);
            size_ = std::exchange(o.size_, 0);
        }
        return *this;
    }

    bool Open(const std::string& path) {
        Reset();
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        struct stat st {};
        if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
            ::close(fd);
            return false;
        }
        void* p = ::mmap(nullptr, static_cast<std::size_t>(st.st_size),
                         PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) return false;
        data_ = static_cast<const unsigned char*>(p);
        size_ = static_cast<std::size_t>(st.st_size);
        return true;
    }

    void Reset() {
        if (data_ != nullptr) {
            ::munmap(const_cast<unsigned char*>(data_), size_);
        }
        data_ = nullptr;
        size_ = 0;
    }

    const unsigned char* data() const { return data_; }
    std::size_t          size() const { return size_; }

   private:
    const unsigned char* data_ = nullptr;
    std::size_t          size_ = 0;
};

// meta.bin length: fixed header + one (offset,size) pair per tensor; 0 if
// the blob is malformed.
std::size_t MetaBytes(const unsigned char* p, std::size_t size,
                      std::size_t count_at, std::size_t header) {
    if (size < header) return 0;
    const std::int32_t n = ReadI32(p + count_at);
    if (n <= 0 || n > 256) return 0;
    const std::size_t meta = header + 8u * static_cast<std::size_t>(n);
    return (meta < size && (size - meta) % 4 == 0) ? meta : 0;
}

// Fixed worker pool behind snt_piperlite_set_parallel. Jobs are pulled
// from a shared counter; the caller thread works too.
class WorkerPool {
   public:
    explicit WorkerPool(int threads) {
        for (int i = 1; i < threads; ++i) {
            workers_.emplace_back([this] { WorkerMain(); });
        }
    }

    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            quit_ = true;
            ++generation_;
        }
        go_.notify_all();
        for (auto& t : workers_) t.join();
    }

    WorkerPool(const WorkerPool&)            = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    static void Run(int n, void (*job)(void*, int), void* ctx, void* user) {
        static_cast<WorkerPool*>(user)->RunJobs(n, job, ctx);
    }

   private:
    void RunJobs(int n, void (*job)(void*, int), void* ctx) {
        if (workers_.empty() || n <= 1) {
            for (int j = 0; j < n; ++j) job(ctx, j);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            job_  = job;
            ctx_  = ctx;
            n_    = n;
            next_.store(0, std::memory_order_relaxed);
            busy_ = static_cast<int>(workers_.size());
            ++generation_;
        }
        go_.notify_all();
        Drain();
        std::unique_lock<std::mutex> lk(mu_);
        done_.wait(lk, [this] { return busy_ == 0; });
    }

    void Drain() {
        int j;
        while ((j = next_.fetch_add(1, std::memory_order_relaxed)) < n_) {
            job_(ctx_, j);
        }
    }

    void WorkerMain() {
        unsigned seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(mu_);
                go_.wait(lk, [&] { return generation_ != seen; });
                seen = generation_;
                if (quit_) return;
            }
            Drain();
            std::lock_guard<std::mutex> lk(mu_);
            if (--busy_ == 0) done_.notify_one();
        }
    }

    std::vector<std::thread> workers_;
    std::mutex               mu_;
    std::condition_variable  go_;
    std::condition_variable  done_;
    unsigned                 generation_ = 0;
    bool                     quit_       = false;
    int                      busy_       = 0;
    void (*job_)(void*, int)             = nullptr;
    void*                    ctx_        = nullptr;
    int                      n_          = 0;
    std::atomic<int>         next_{0};
};

// Same framing as upstream snt_g2p_text_to_ids(): BOS PAD (id PAD)* EOS,
// clause punctuation kept, parenthesized language flags dropped.
int TextToIds(const std::string& text, std::vector<std::int32_t>& ids) {
    ids.clear();
    ids.push_back(kIdBos);
    ids.push_back(kIdPad);
    const auto push = [&ids](int id) {
        if (id < 0) return;
        ids.push_back(id);
        ids.push_back(kIdPad);
    };

    const void* tp = text.c_str();
    int guard = 0;
    while (tp != nullptr && *static_cast<const char*>(tp) != '\0' &&
           guard++ < 4096) {
        const char* p0 = static_cast<const char*>(tp);
        const char* ph = espeak_TextToPhonemes(&tp, espeakCHARS_UTF8,
                                               espeakPHONEMES_IPA);
        const char* p1 = tp != nullptr ? static_cast<const char*>(tp)
                                       : p0 + std::strlen(p0);
        if (ph != nullptr) {
            bool in_flag = false;
            for (const char* p = ph; *p != '\0';) {
                unsigned cp = 0;
                p = NextCodepoint(p, &cp);
                if (cp == '(') { in_flag = true; continue; }
                if (cp == ')') { in_flag = false; continue; }
                if (in_flag) continue;
                push(LookupId(cp));
            }
        }
        // espeak drops clause punctuation; restore the terminator.
        char term = 0;
        for (const char* q = p1 - 1; q >= p0; --q) {
            const char c = *q;
            if (c == '.' || c == ',' || c == '?' || c == '!' || c == ':' ||
                c == ';') {
                term = c;
                break;
            }
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '-') {
                break;
            }
        }
        if (term != 0) {
            push(LookupId(static_cast<unsigned char>(term)));
            if (term == ',' || term == ':' || term == ';') push(LookupId(' '));
        }
    }
    ids.push_back(kIdEos);
    return static_cast<int>(ids.size());
}

}  // namespace

struct SanoTts::Impl {
    MappedFile                  front_file;
    MappedFile                  dec_file;
    snt_front_model             front{};
    snt_piperlite_model         dec{};
    std::unique_ptr<WorkerPool> pool;

    bool LoadVoice(const std::string& voice_dir);
};

// Both files are mmapped and used in place (weights stay page cache).
bool SanoTts::Impl::LoadVoice(const std::string& voice_dir) {
    const std::filesystem::path dir(voice_dir);
    if (!front_file.Open((dir / "front_f32.bin").string())) {
        LVA_LOGE(kTag, "cannot open %s/front_f32.bin", dir.c_str());
        return false;
    }
    if (!dec_file.Open((dir / "dec_q8.bin").string())) {
        LVA_LOGE(kTag, "cannot open %s/dec_q8.bin", dir.c_str());
        return false;
    }
    // front_f32.bin: upstream meta.bin (tensor count at byte 68, 72-byte
    // header) followed by the fp32 weights.
    const std::size_t meta =
        MetaBytes(front_file.data(), front_file.size(), 68, 72);
    if (meta == 0) {
        LVA_LOGE(kTag, "malformed front_f32.bin");
        return false;
    }
    int rc = snt_front_init(
        &front, front_file.data(), meta,
        reinterpret_cast<const float*>(front_file.data() + meta),
        (front_file.size() - meta) / 4);
    if (rc != 0) {
        LVA_LOGE(kTag, "front init failed (%d)", rc);
        return false;
    }
    rc = snt_piperlite_init_q8(&dec, dec_file.data(), dec_file.size());
    if (rc != 0) {
        LVA_LOGE(kTag, "decoder init failed (%d)", rc);
        return false;
    }
    if (dec.in_ch != front.a_out) {
        LVA_LOGE(kTag, "front/decoder mismatch");
        return false;
    }
    return true;
}


SanoTts::SanoTts(Config config) : config_(std::move(config)) {}

SanoTts::~SanoTts() { Stop(); }

int SanoTts::SampleRate() const { return impl_ ? kSampleRate : 0; }

bool SanoTts::Start() {
    if (impl_) return true;

    if (!std::filesystem::exists(
            std::filesystem::path(config_.espeak_data_dir) / "phontab")) {
        LVA_LOGE(kTag, "espeak-ng data missing under %s",
                 config_.espeak_data_dir.c_str());
        return false;
    }

    auto impl = std::make_unique<Impl>();
    if (!impl->LoadVoice(config_.voice_dir)) return false;

    // Without DONT_EXIT a bad data dir calls exit(1).
    if (espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0,
                          config_.espeak_data_dir.c_str(),
                          espeakINITIALIZE_DONT_EXIT) <= 0) {
        LVA_LOGE(kTag, "espeak_Initialize failed (data=%s)",
                 config_.espeak_data_dir.c_str());
        return false;
    }
    if (espeak_SetVoiceByName(kEspeakVoice) != EE_OK) {
        LVA_LOGE(kTag, "espeak voice '%s' not found", kEspeakVoice);
        espeak_Terminate();
        return false;
    }

    const int threads = std::clamp(config_.threads, 1, 4);
    impl->pool = std::make_unique<WorkerPool>(threads);
    snt_piperlite_set_parallel(&WorkerPool::Run, impl->pool.get());

    impl_ = std::move(impl);
    LVA_LOGI(kTag, "started (speed=%.2f, threads=%d)", config_.speed,
             threads);
    return true;
}

void SanoTts::Stop() {
    if (!impl_) return;
    snt_piperlite_set_parallel(nullptr, nullptr);
    impl_.reset();
    espeak_Terminate();
    LVA_LOGI(kTag, "stopped");
}

void SanoTts::SetSpeed(float speed) {
    config_.speed = speed;
    LVA_LOGI(kTag, "speed=%.2f", config_.speed);
}

bool SanoTts::Synthesize(const std::string& text,
                         std::vector<std::int16_t>& out_pcm) {
    out_pcm.clear();
    if (!impl_ || text.empty()) return false;
    Impl& m = *impl_;
    const auto t0 = std::chrono::steady_clock::now();

    std::vector<std::int32_t> ids;
    const int n = TextToIds(text, ids);
    if (n <= 3) {
        LVA_LOGW(kTag, "no phonemes for '%s'", text.c_str());
        return false;
    }

    // Students can have a smaller vocabulary than the G2P table.
    std::vector<std::int32_t> local_ids(ids.size());
    const auto clamp_ids = [&](int vocab) {
        for (std::size_t i = 0; i < ids.size(); ++i) {
            local_ids[i] = (ids[i] >= 0 && ids[i] < vocab)
                               ? ids[i]
                               : (kIdFallback < vocab ? kIdFallback : 0);
        }
    };

    const float length_scale =
        kBaseLengthScale / std::clamp(config_.speed, 0.5f, 2.0f);

    std::vector<std::int32_t> durations(ids.size());
    long frames = 0;
    {
        clamp_ids(m.front.d_vocab);
        std::vector<float> arena(
            std::max<std::size_t>(1, snt_front_duration_arena_floats(&m.front, n)));
        frames = snt_front_durations(&m.front, local_ids.data(), n,
                                     length_scale, durations.data(),
                                     arena.data(), arena.size());
    }
    if (frames <= 0) {
        LVA_LOGE(kTag, "duration model failed (%ld)", frames);
        return false;
    }

    std::vector<float> latent(static_cast<std::size_t>(m.front.a_out) *
                              static_cast<std::size_t>(frames));
    {
        clamp_ids(m.front.a_vocab);
        std::vector<float> arena(std::max<std::size_t>(
            1, snt_front_latent_arena_floats(&m.front, n, frames)));
        const int rc = snt_front_latent(&m.front, local_ids.data(),
                                        durations.data(), n, frames,
                                        latent.data(), arena.data(),
                                        arena.size());
        if (rc != 0) {
            LVA_LOGE(kTag, "acoustic model failed (%d)", rc);
            return false;
        }
    }
    const auto t1 = std::chrono::steady_clock::now();

    // Decode in overlapping chunks; keep only each chunk's core.
    const int  in_ch     = m.dec.in_ch;
    const int  max_width = kChunkFrames + 2 * kOverlapFrames;
    std::vector<float> arena(
        snt_piperlite_arena_floats(&m.dec, max_width));
    std::vector<float> z(static_cast<std::size_t>(in_ch) * max_width);
    std::vector<float> audio(static_cast<std::size_t>(max_width) * kHop);
    out_pcm.resize(static_cast<std::size_t>(frames) * kHop);

    for (long s = 0; s < frames; s += kChunkFrames) {
        const long a = std::max(0L, s - kOverlapFrames);
        const long b = std::min(frames, s + kChunkFrames + kOverlapFrames);
        const int  w = static_cast<int>(b - a);
        for (int c = 0; c < in_ch; ++c) {
            std::memcpy(z.data() + static_cast<std::size_t>(c) * w,
                        latent.data() + static_cast<std::size_t>(c) * frames + a,
                        static_cast<std::size_t>(w) * sizeof(float));
        }
        const int rc = snt_piperlite_synthesize(&m.dec, z.data(), w,
                                                audio.data(), arena.data(),
                                                arena.size());
        if (rc != 0) {
            LVA_LOGE(kTag, "decoder failed (%d)", rc);
            out_pcm.clear();
            return false;
        }
        const long e = std::min(frames, s + kChunkFrames);
        for (long j = s * kHop; j < e * kHop; ++j) {
            const float v = audio[static_cast<std::size_t>(j - a * kHop)] *
                            32767.0f;
            out_pcm[static_cast<std::size_t>(j)] = static_cast<std::int16_t>(
                std::lrintf(std::clamp(v, -32768.0f, 32767.0f)));
        }
    }

    const auto t2 = std::chrono::steady_clock::now();
    const double audio_s = static_cast<double>(out_pcm.size()) / kSampleRate;
    const double front_s = std::chrono::duration<double>(t1 - t0).count();
    const double dec_s   = std::chrono::duration<double>(t2 - t1).count();
    LVA_LOGI(kTag, "%d ids -> %.2f s audio (front %.0f ms, decoder %.0f ms, "
                   "RTF %.2f)",
             n, audio_s, front_s * 1000.0, dec_s * 1000.0,
             (front_s + dec_s) / std::max(audio_s, 1e-3));
    return true;
}

}  // namespace lva::audio
