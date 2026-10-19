// Hardware-loopback AEC check. Captures hw:0,4 (4 ch), runs the WebRTC APM
// and writes raw_mic.wav / ref.wav / aec_out.wav, printing ERLE per second.
// Opens its own ALSA handle; stop linux-voice-assistant-cpp first.
//
//   aec_loopback_test --out /tmp/aec
//   aec_loopback_test --device hw:0,4 --mic-ch 0 --ref-ch 2 --out /tmp/aec
//   aec_loopback_test --no-aec --out /tmp/aec

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <alsa/asoundlib.h>

#include "modules/audio_processing/include/audio_processing.h"

namespace {

constexpr unsigned kSampleRate    = 16000;
constexpr unsigned kFrameSamples  = 160;     // 10 ms @ 16 kHz
constexpr unsigned kNumChannels   = 4;       // ch1..ch4 (1-indexed in user terms)

std::atomic<bool> g_stop{false};

extern "C" void OnSignal(int /*signo*/) {
    g_stop.store(true, std::memory_order_relaxed);
}

struct Options {
    std::string device      = "hw:0,4";
    std::string out_dir     = "/tmp/aec";
    int         duration_s  = 5;
    int         mic_ch      = 0;     // 0-based; user "ch1" = 0
    int         ref_a       = 2;     // 0-based; user "ch3" = 2
    int         ref_b       = 3;     // 0-based; user "ch4" = 3 (set -1 to disable)
    bool        aec_enabled = true;
    int         ns_level    = 2;     // 0=off, 1..4 = APM NS level
    bool        hpf_enabled = true;
    int         stream_delay_ms = 0;
    int         agc_db          = 0;    // production fixed-digital AGC, 0=off
    int         mic_volume      = 100;  // post-APM percent multiplier, 100=unity

    // --play: the Amlogic loopback DAI needs playback DMA running before the
    // capture opens, otherwise readi stalls and then fails with EIO.
    std::string play_wav;
    std::string play_device   = "hw:0,1";
    int         play_delay_ms = 200;
};

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "  --device <dev>          ALSA capture device (default: hw:0,4)\n"
        "  --out <dir>             Output dir for WAV files (default: /tmp/aec)\n"
        "  --duration <sec>        Capture duration seconds (default: 5)\n"
        "  --mic-ch <0-3>          0-based mic channel (default: 0 = user ch1)\n"
        "  --ref-ch <0-3>          0-based first ref channel (default: 2 = ch3)\n"
        "  --ref-ch2 <0-3 | -1>    0-based second ref channel; -1 = single ref\n"
        "                          (default: 3 = ch4, downmixed to mono)\n"
        "  --no-aec                Disable echo canceller (capture-only test)\n"
        "  --no-hpf                Disable high-pass filter\n"
        "  --ns-level <0-4>        Noise suppression level (default 2; 0=off)\n"
        "  --stream-delay-ms <n>   Hint to AEC; 0 is right for hw loopback\n"
        "  --agc-db <0-31>          Fixed-digital WebRTC AGC gain (default 0)\n"
        "  --mic-volume <1-4000>    Post-APM percent gain (default 100)\n"
        "  --play <file.wav>       Spawn aplay on this WAV before opening\n"
        "                          capture (loops in a child process; killed\n"
        "                          on exit). Loopback DAI requires playback\n"
        "                          DMA already active when capture opens.\n"
        "  --play-device <dev>     ALSA playback device for --play\n"
        "                          (default: hw:0,1)\n"
        "  --play-delay-ms <n>     Sleep between aplay start and capture\n"
        "                          open (default: 200 ms)\n"
        "  -h, --help              This help.\n",
        argv0);
}

bool ParseCli(int argc, char** argv, Options& out) {
    static const struct option longopts[] = {
        {"device",          required_argument, nullptr, 'd'},
        {"out",             required_argument, nullptr, 'o'},
        {"duration",        required_argument, nullptr, 't'},
        {"mic-ch",          required_argument, nullptr, 'm'},
        {"ref-ch",          required_argument, nullptr, 'r'},
        {"ref-ch2",         required_argument, nullptr, 's'},
        {"no-aec",          no_argument,       nullptr, 'A'},
        {"no-hpf",          no_argument,       nullptr, 'H'},
        {"ns-level",        required_argument, nullptr, 'n'},
        {"stream-delay-ms", required_argument, nullptr, 'D'},
        {"agc-db",          required_argument, nullptr, 'G'},
        {"mic-volume",      required_argument, nullptr, 'M'},
        {"play",            required_argument, nullptr, 'P'},
        {"play-device",     required_argument, nullptr, 'V'},
        {"play-delay-ms",   required_argument, nullptr, 'L'},
        {"help",            no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };
    int c;
    while ((c = ::getopt_long(argc, argv, "h", longopts, nullptr)) != -1) {
        switch (c) {
            case 'd': out.device = optarg; break;
            case 'o': out.out_dir = optarg; break;
            case 't': out.duration_s = std::atoi(optarg); break;
            case 'm': out.mic_ch = std::atoi(optarg); break;
            case 'r': out.ref_a = std::atoi(optarg); break;
            case 's': out.ref_b = std::atoi(optarg); break;
            case 'A': out.aec_enabled = false; break;
            case 'H': out.hpf_enabled = false; break;
            case 'n': out.ns_level = std::atoi(optarg); break;
            case 'D': out.stream_delay_ms = std::atoi(optarg); break;
            case 'G': out.agc_db = std::atoi(optarg); break;
            case 'M': out.mic_volume = std::atoi(optarg); break;
            case 'P': out.play_wav = optarg; break;
            case 'V': out.play_device = optarg; break;
            case 'L': out.play_delay_ms = std::atoi(optarg); break;
            case 'h':
                PrintUsage(argv[0]);
                std::exit(0);
            default:
                PrintUsage(argv[0]);
                return false;
        }
    }
    if (out.duration_s <= 0 || out.duration_s > 3600) {
        std::fprintf(stderr, "invalid --duration\n");
        return false;
    }
    if (out.mic_ch < 0 || out.mic_ch >= static_cast<int>(kNumChannels)) {
        std::fprintf(stderr, "--mic-ch out of range [0..%u]\n",
                     kNumChannels - 1);
        return false;
    }
    if (out.ref_a < 0 || out.ref_a >= static_cast<int>(kNumChannels)) {
        std::fprintf(stderr, "--ref-ch out of range\n");
        return false;
    }
    if (out.ref_b != -1 &&
        (out.ref_b < 0 || out.ref_b >= static_cast<int>(kNumChannels))) {
        std::fprintf(stderr, "--ref-ch2 out of range\n");
        return false;
    }
    if (out.ns_level < 0 || out.ns_level > 4) {
        std::fprintf(stderr, "--ns-level out of range\n");
        return false;
    }
    if (out.agc_db < 0 || out.agc_db > 31) {
        std::fprintf(stderr, "--agc-db out of range [0..31]\n");
        return false;
    }
    if (out.mic_volume < 1 || out.mic_volume > 4000) {
        std::fprintf(stderr, "--mic-volume out of range [1..4000]\n");
        return false;
    }
    return true;
}

class WavWriter {
   public:
    bool Open(const std::string& path) {
        path_ = path;
        fp_ = std::fopen(path.c_str(), "wb");
        if (fp_ == nullptr) {
            std::fprintf(stderr, "WavWriter: open %s failed: %s\n",
                         path.c_str(), std::strerror(errno));
            return false;
        }
        std::uint8_t hdr[44] = {0};
        std::fwrite(hdr, 1, sizeof(hdr), fp_);
        return true;
    }

    void Write(const std::int16_t* data, std::size_t n_samples) {
        if (fp_ == nullptr) return;
        std::fwrite(data, sizeof(std::int16_t), n_samples, fp_);
        samples_ += n_samples;
    }

    void Close() {
        if (fp_ == nullptr) return;
        const std::uint32_t data_bytes =
            static_cast<std::uint32_t>(samples_ * sizeof(std::int16_t));
        const std::uint32_t riff_bytes = 36 + data_bytes;
        std::uint8_t hdr[44] = {0};
        std::memcpy(hdr +  0, "RIFF", 4);
        hdr[ 4] = static_cast<std::uint8_t>(riff_bytes        & 0xff);
        hdr[ 5] = static_cast<std::uint8_t>((riff_bytes >>  8) & 0xff);
        hdr[ 6] = static_cast<std::uint8_t>((riff_bytes >> 16) & 0xff);
        hdr[ 7] = static_cast<std::uint8_t>((riff_bytes >> 24) & 0xff);
        std::memcpy(hdr +  8, "WAVE", 4);
        std::memcpy(hdr + 12, "fmt ", 4);
        hdr[16] = 16;             // fmt chunk size
        hdr[20] = 1;              // PCM
        hdr[22] = 1;              // mono
        const std::uint32_t sr = kSampleRate;
        hdr[24] = static_cast<std::uint8_t>(sr        & 0xff);
        hdr[25] = static_cast<std::uint8_t>((sr >>  8) & 0xff);
        hdr[26] = static_cast<std::uint8_t>((sr >> 16) & 0xff);
        hdr[27] = static_cast<std::uint8_t>((sr >> 24) & 0xff);
        const std::uint32_t byte_rate = sr * 2; // 1ch * 2 bytes
        hdr[28] = static_cast<std::uint8_t>(byte_rate        & 0xff);
        hdr[29] = static_cast<std::uint8_t>((byte_rate >>  8) & 0xff);
        hdr[30] = static_cast<std::uint8_t>((byte_rate >> 16) & 0xff);
        hdr[31] = static_cast<std::uint8_t>((byte_rate >> 24) & 0xff);
        hdr[32] = 2;              // block align
        hdr[34] = 16;             // bits/sample
        std::memcpy(hdr + 36, "data", 4);
        hdr[40] = static_cast<std::uint8_t>(data_bytes        & 0xff);
        hdr[41] = static_cast<std::uint8_t>((data_bytes >>  8) & 0xff);
        hdr[42] = static_cast<std::uint8_t>((data_bytes >> 16) & 0xff);
        hdr[43] = static_cast<std::uint8_t>((data_bytes >> 24) & 0xff);

        std::fseek(fp_, 0, SEEK_SET);
        std::fwrite(hdr, 1, sizeof(hdr), fp_);
        std::fclose(fp_);
        fp_ = nullptr;
    }

    ~WavWriter() { Close(); }

    const std::string& path() const { return path_; }

   private:
    std::FILE*  fp_     = nullptr;
    std::size_t samples_ = 0;
    std::string path_;
};

double SumSquares(const std::int16_t* x, std::size_t n) {
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = static_cast<double>(x[i]);
        s += v * v;
    }
    return s;
}

double Rms(const std::int16_t* x, std::size_t n) {
    if (n == 0) return 0.0;
    return std::sqrt(SumSquares(x, n) / static_cast<double>(n));
}

bool OpenAlsaCapture(const std::string& device, snd_pcm_t** out_pcm) {
    snd_pcm_t* pcm = nullptr;
    int rc = ::snd_pcm_open(&pcm, device.c_str(),
                            SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
        std::fprintf(stderr, "snd_pcm_open(%s) failed: %s\n",
                     device.c_str(), ::snd_strerror(rc));
        return false;
    }

    // Explicit hw_params like arecord: snd_pcm_set_params() adds a resampler
    // and a tiny buffer, and the loopback capture then never delivers frames.
    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);

    if ((rc = ::snd_pcm_hw_params_any(pcm, hw)) < 0) {
        std::fprintf(stderr, "hw_params_any: %s\n", ::snd_strerror(rc));
        ::snd_pcm_close(pcm); return false;
    }
    if ((rc = ::snd_pcm_hw_params_set_access(
             pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        std::fprintf(stderr, "set_access: %s\n", ::snd_strerror(rc));
        ::snd_pcm_close(pcm); return false;
    }
    if ((rc = ::snd_pcm_hw_params_set_format(
             pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0) {
        std::fprintf(stderr, "set_format S16_LE: %s\n", ::snd_strerror(rc));
        ::snd_pcm_close(pcm); return false;
    }
    if ((rc = ::snd_pcm_hw_params_set_channels(
             pcm, hw, kNumChannels)) < 0) {
        std::fprintf(stderr, "set_channels %u: %s\n",
                     kNumChannels, ::snd_strerror(rc));
        ::snd_pcm_close(pcm); return false;
    }
    unsigned rate = kSampleRate;
    if ((rc = ::snd_pcm_hw_params_set_rate_near(
             pcm, hw, &rate, nullptr)) < 0) {
        std::fprintf(stderr, "set_rate_near %u: %s\n",
                     kSampleRate, ::snd_strerror(rc));
        ::snd_pcm_close(pcm); return false;
    }
    if (rate != kSampleRate) {
        std::fprintf(stderr,
                     "[alsa] WARNING: device gave us rate %u, expected %u\n",
                     rate, kSampleRate);
    }

    snd_pcm_uframes_t period_frames = kFrameSamples;
    if ((rc = ::snd_pcm_hw_params_set_period_size_near(
             pcm, hw, &period_frames, nullptr)) < 0) {
        std::fprintf(stderr, "set_period_size_near: %s\n",
                     ::snd_strerror(rc));
        ::snd_pcm_close(pcm); return false;
    }
    snd_pcm_uframes_t buffer_frames = kSampleRate / 2;  // 500 ms
    if ((rc = ::snd_pcm_hw_params_set_buffer_size_near(
             pcm, hw, &buffer_frames)) < 0) {
        std::fprintf(stderr, "set_buffer_size_near: %s\n",
                     ::snd_strerror(rc));
        ::snd_pcm_close(pcm); return false;
    }

    if ((rc = ::snd_pcm_hw_params(pcm, hw)) < 0) {
        std::fprintf(stderr, "snd_pcm_hw_params (commit): %s\n",
                     ::snd_strerror(rc));
        ::snd_pcm_close(pcm); return false;
    }

    std::fprintf(stderr,
                 "[alsa] device=%s rate=%u ch=%u period=%lu buffer=%lu\n",
                 device.c_str(), rate, kNumChannels,
                 static_cast<unsigned long>(period_frames),
                 static_cast<unsigned long>(buffer_frames));

    *out_pcm = pcm;
    return true;
}

// The loopback DAI returns EIO while nothing is playing; treat it like an
// xrun and retry, so playback can start after the tool.
static constexpr int kReadIoRetries  = 50;     // ~5 s at 100 ms each
static constexpr int kRetrySleepMs   = 100;

snd_pcm_sframes_t ReadFrame(snd_pcm_t* pcm,
                            std::int16_t* interleaved,
                            std::size_t   frames) {
    for (int attempt = 0;; ++attempt) {
        snd_pcm_sframes_t r = ::snd_pcm_readi(pcm, interleaved, frames);
        if (r >= 0) {
            return r;
        }
        const int err = static_cast<int>(r);
        const bool retryable =
            (err == -EPIPE || err == -ESTRPIPE || err == -EINTR ||
             err == -EIO);
        if (!retryable || attempt >= kReadIoRetries) {
            std::fprintf(stderr, "[alsa] readi: %s (attempt=%d)\n",
                         ::snd_strerror(err), attempt);
            return 0;
        }
        if (g_stop.load(std::memory_order_relaxed)) return 0;

        std::fprintf(stderr,
                     "[alsa] %s; recovering (attempt %d/%d)\n",
                     ::snd_strerror(err), attempt + 1, kReadIoRetries);
        const int rc = ::snd_pcm_recover(pcm, err, /*silent=*/1);
        if (rc < 0) {
            std::fprintf(stderr, "[alsa] recover failed: %s\n",
                         ::snd_strerror(rc));
            return 0;
        }
        ::usleep(static_cast<useconds_t>(kRetrySleepMs) * 1000);
    }
}

void SplitInterleaved(const std::int16_t* il,
                      std::size_t          frames,
                      int                  mic_ch,
                      int                  ref_a,
                      int                  ref_b,
                      std::int16_t*        mic_out,
                      std::int16_t*        ref_out) {
    const bool single_ref = (ref_b < 0);
    for (std::size_t i = 0; i < frames; ++i) {
        const std::int16_t* row = il + i * kNumChannels;
        mic_out[i] = row[mic_ch];
        if (single_ref) {
            ref_out[i] = row[ref_a];
        } else {
            const int32_t s = static_cast<int32_t>(row[ref_a]) +
                              static_cast<int32_t>(row[ref_b]);
            ref_out[i] = static_cast<std::int16_t>(s >> 1);
        }
    }
}

// Single-shot on purpose: restarting aplay tears down the codec frddrs and
// breaks the loopback capture.
pid_t SpawnAplayLoop(const std::string& device, const std::string& wav) {
    const pid_t pid = ::fork();
    if (pid < 0) {
        std::fprintf(stderr, "fork failed: %s\n", std::strerror(errno));
        return -1;
    }
    if (pid > 0) {
        std::fprintf(stderr,
                     "[play] spawned aplay pid=%d device=%s wav=%s\n",
                     static_cast<int>(pid), device.c_str(), wav.c_str());
        return pid;
    }

    int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        ::dup2(devnull, STDOUT_FILENO);
        ::dup2(devnull, STDERR_FILENO);
        ::close(devnull);
    }
    struct sigaction dfl{};
    dfl.sa_handler = SIG_DFL;
    ::sigaction(SIGINT,  &dfl, nullptr);
    ::sigaction(SIGTERM, &dfl, nullptr);

    ::execlp("aplay", "aplay",
             "-D", device.c_str(),
             "-q",
             wav.c_str(),
             static_cast<char*>(nullptr));
    ::_exit(127);
}

void KillChild(pid_t pid) {
    if (pid <= 0) return;
    ::kill(pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        int status = 0;
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) return;
        if (r < 0)    return;
        ::usleep(50'000);  // 50 ms
    }
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
}

}  // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!ParseCli(argc, argv, opts)) return 2;

    std::error_code ec;
    std::filesystem::create_directories(opts.out_dir, ec);
    if (ec) {
        std::fprintf(stderr, "failed to create %s: %s\n",
                     opts.out_dir.c_str(), ec.message().c_str());
        return 1;
    }

    struct sigaction sa{};
    sa.sa_handler = &OnSignal;
    ::sigaction(SIGINT,  &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    std::fprintf(stderr,
                 "aec_loopback_test: device=%s out=%s duration=%ds "
                 "mic_ch=%d ref=(%d,%d) aec=%d hpf=%d ns=%d delay=%dms\n",
                 opts.device.c_str(), opts.out_dir.c_str(), opts.duration_s,
                 opts.mic_ch, opts.ref_a, opts.ref_b,
                 opts.aec_enabled ? 1 : 0,
                 opts.hpf_enabled ? 1 : 0,
                 opts.ns_level, opts.stream_delay_ms);

    // Playback must be running before the capture opens (see --play).
    pid_t aplay_pid = -1;
    if (!opts.play_wav.empty()) {
        aplay_pid = SpawnAplayLoop(opts.play_device, opts.play_wav);
        if (aplay_pid < 0) {
            return 1;
        }
        std::fprintf(stderr, "[play] sleeping %d ms before capture open\n",
                     opts.play_delay_ms);
        ::usleep(static_cast<useconds_t>(opts.play_delay_ms) * 1000);
    }

    snd_pcm_t* pcm = nullptr;
    if (!OpenAlsaCapture(opts.device, &pcm)) {
        if (aplay_pid > 0) KillChild(aplay_pid);
        return 1;
    }

    auto* apm = webrtc::AudioProcessingBuilder().Create();
    if (apm == nullptr) {
        std::fprintf(stderr, "AudioProcessingBuilder::Create failed\n");
        ::snd_pcm_close(pcm);
        return 1;
    }

    {
        webrtc::AudioProcessing::Config cfg;
        cfg.echo_canceller.enabled = opts.aec_enabled;
        cfg.echo_canceller.mobile_mode = false;
        cfg.echo_canceller.enforce_high_pass_filtering = true;
        cfg.high_pass_filter.enabled = opts.hpf_enabled;
        if (opts.ns_level > 0) {
            cfg.noise_suppression.enabled = true;
            using L = webrtc::AudioProcessing::Config::NoiseSuppression::Level;
            switch (opts.ns_level) {
                case 1: cfg.noise_suppression.level = L::kLow;       break;
                case 2: cfg.noise_suppression.level = L::kModerate;  break;
                case 3: cfg.noise_suppression.level = L::kHigh;      break;
                case 4: cfg.noise_suppression.level = L::kVeryHigh;  break;
            }
        }
        if (opts.agc_db > 0) {
            cfg.gain_controller1.enabled = true;
            cfg.gain_controller1.mode =
                webrtc::AudioProcessing::Config::GainController1::kFixedDigital;
            cfg.gain_controller1.target_level_dbfs = 3;
            cfg.gain_controller1.compression_gain_db =
                std::clamp(opts.agc_db, 1, 31);
            cfg.gain_controller1.enable_limiter = true;
        }
        apm->ApplyConfig(cfg);
    }

    const webrtc::StreamConfig stream_cfg(
        kSampleRate, /*num_channels=*/1, /*has_keyboard=*/false);

    WavWriter wav_mic, wav_ref, wav_out, wav_final;
    if (!wav_mic.Open(opts.out_dir + "/raw_mic.wav") ||
        !wav_ref.Open(opts.out_dir + "/ref.wav")    ||
        !wav_out.Open(opts.out_dir + "/aec_out.wav") ||
        !wav_final.Open(opts.out_dir + "/production_out.wav")) {
        ::snd_pcm_close(pcm);
        return 1;
    }

    std::vector<std::int16_t> il_buf(kNumChannels * kFrameSamples);
    std::vector<std::int16_t> mic(kFrameSamples);
    std::vector<std::int16_t> ref(kFrameSamples);
    std::vector<std::int16_t> out(kFrameSamples);
    std::vector<std::int16_t> final_out(kFrameSamples);

    const std::size_t target_periods =
        static_cast<std::size_t>(opts.duration_s) * (kSampleRate / kFrameSamples);
    std::size_t periods_done = 0;

    double sec_mic_e = 0.0, sec_out_e = 0.0, sec_ref_e = 0.0;
    std::size_t sec_periods = 0;
    constexpr std::size_t kPeriodsPerSec = kSampleRate / kFrameSamples; // 100

    double tot_mic_e_loud = 0.0, tot_out_e_loud = 0.0;
    std::size_t tot_loud_periods = 0;

    // ERLE is only meaningful while the ref carries playback.
    constexpr double kRefRmsLoud = 200.0;

    std::fprintf(stderr,
                 "[run] capturing... start playback now if you haven't "
                 "(EIO from a quiet hw:0,4 will be retried up to %d times "
                 "/ %d ms each)\n",
                 kReadIoRetries, kRetrySleepMs);

    while (!g_stop.load(std::memory_order_relaxed) &&
           periods_done < target_periods) {

        const snd_pcm_sframes_t r = ReadFrame(pcm, il_buf.data(),
                                              kFrameSamples);
        if (r <= 0) break;
        if (static_cast<std::size_t>(r) != kFrameSamples) {
            std::fprintf(stderr, "[alsa] short read: %ld\n",
                         static_cast<long>(r));
            std::memset(il_buf.data() + r * kNumChannels, 0,
                        (kFrameSamples - r) * kNumChannels *
                        sizeof(std::int16_t));
        }

        SplitInterleaved(il_buf.data(), kFrameSamples,
                         opts.mic_ch, opts.ref_a, opts.ref_b,
                         mic.data(), ref.data());

        wav_mic.Write(mic.data(), kFrameSamples);
        wav_ref.Write(ref.data(), kFrameSamples);

        // Reverse stream first within a period.
        const int rrc = apm->ProcessReverseStream(
            ref.data(), stream_cfg, stream_cfg, ref.data());
        if (rrc != webrtc::AudioProcessing::kNoError) {
            std::fprintf(stderr, "ProcessReverseStream rc=%d\n", rrc);
        }
        if (opts.aec_enabled) {
            apm->set_stream_delay_ms(opts.stream_delay_ms);
        }
        const int prc = apm->ProcessStream(
            mic.data(), stream_cfg, stream_cfg, out.data());
        if (prc != webrtc::AudioProcessing::kNoError) {
            std::fprintf(stderr, "ProcessStream rc=%d\n", prc);
        }

        wav_out.Write(out.data(), kFrameSamples);

        // Same gain as AudioCapture::PostFrame().
        final_out = out;
        if (opts.mic_volume > 0 && opts.mic_volume != 100) {
            const float gain = static_cast<float>(opts.mic_volume) / 100.0f;
            for (std::size_t i = 0; i < final_out.size(); ++i) {
                const long scaled =
                    std::lround(static_cast<float>(final_out[i]) * gain);
                const long clamped =
                    std::clamp(scaled, -32768L, 32767L);
                final_out[i] = static_cast<std::int16_t>(clamped);
            }
        }
        wav_final.Write(final_out.data(), kFrameSamples);

        const double mic_e = SumSquares(mic.data(), kFrameSamples);
        const double out_e = SumSquares(out.data(), kFrameSamples);
        const double ref_e = SumSquares(ref.data(), kFrameSamples);

        sec_mic_e += mic_e;
        sec_out_e += out_e;
        sec_ref_e += ref_e;
        ++sec_periods;

        const double ref_rms_period = std::sqrt(
            ref_e / static_cast<double>(kFrameSamples));
        if (ref_rms_period >= kRefRmsLoud) {
            tot_mic_e_loud += mic_e;
            tot_out_e_loud += out_e;
            ++tot_loud_periods;
        }

        ++periods_done;

        if (sec_periods >= kPeriodsPerSec) {
            const double mic_rms = std::sqrt(
                sec_mic_e / static_cast<double>(sec_periods * kFrameSamples));
            const double out_rms = std::sqrt(
                sec_out_e / static_cast<double>(sec_periods * kFrameSamples));
            const double ref_rms = std::sqrt(
                sec_ref_e / static_cast<double>(sec_periods * kFrameSamples));
            double erle_db = 0.0;
            const bool ref_loud = (ref_rms >= kRefRmsLoud);
            if (ref_loud && sec_out_e > 0.0) {
                erle_db = 10.0 * std::log10(sec_mic_e / sec_out_e);
            }
            std::fprintf(stderr,
                         "[t=%4ds] mic_rms=%6.0f ref_rms=%6.0f out_rms=%6.0f "
                         "ERLE=%5.1f dB %s\n",
                         static_cast<int>(periods_done / kPeriodsPerSec),
                         mic_rms, ref_rms, out_rms,
                         ref_loud ? erle_db : 0.0,
                         ref_loud ? "" : "(ref silent — ERLE n/a)");
            sec_mic_e = sec_out_e = sec_ref_e = 0.0;
            sec_periods = 0;
        }
    }

    wav_mic.Close();
    wav_ref.Close();
    wav_out.Close();
    ::snd_pcm_close(pcm);
    apm->Release();

    if (aplay_pid > 0) {
        std::fprintf(stderr, "[play] killing aplay pid=%d\n",
                     static_cast<int>(aplay_pid));
        KillChild(aplay_pid);
    }

    if (tot_loud_periods > 0) {
        const double erle_db =
            10.0 * std::log10(tot_mic_e_loud /
                              std::max(1e-9, tot_out_e_loud));
        std::fprintf(stderr,
                     "\nFinal ERLE over loud periods: %.1f dB "
                     "(%zu periods = %.1fs of playback)\n",
                     erle_db, tot_loud_periods,
                     static_cast<double>(tot_loud_periods) / kPeriodsPerSec);
    } else {
        std::fprintf(stderr,
                     "\nNo loud-reference periods detected — start playback "
                     "(mpv / TTS / sendspin) before running, then re-run.\n");
    }
    std::fprintf(stderr,
                 "WAV output: %s, %s, %s\n",
                 wav_mic.path().c_str(),
                 wav_ref.path().c_str(),
                 wav_out.path().c_str());
    return 0;
}
