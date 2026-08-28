#pragma once

// Captures the system output mix and turns it into the numbers a visualization
// can draw: a log-spaced band spectrum, four summary energies, and discrete
// onset events.
//
// Nothing here knows about this demo. It needs PulseAudio and the standard
// library, not Qt, not the agent ABI, and not the cube, so it compiles and runs
// on its own against any program that wants the same numbers. It sits in
// snippets/ next to cube_mesh.h and scene_toggle.h, which are both deliberately
// specific to this application, and the neighbours are the only thing about it
// that suggests otherwise.
//
// The capture runs on its own thread because pa_simple_read() blocks, and the
// analysis runs there too so the GUI thread only ever copies a small result
// struct. Everything the render side reads goes through snapshot(), which is
// the one place the mutex is taken.
//
// Stopping never joins. pa_simple_read() blocks until its fragment is full, and
// a sink that suspends mid-read can leave that call parked for as long as it
// stays suspended; joining there would freeze the GUI thread. Instead the
// analyzer is owned by shared_ptr, the thread holds a copy, and requestStop()
// just sets a flag. The thread notices after its next read and drops its
// reference, so the state outlives the module's handle to it and nothing is
// destroyed underneath a blocked call.

#include <pulse/error.h>
#include <pulse/simple.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace audio_analysis {

constexpr int sampleRate = 48000;
constexpr int fftSize = 2048;
constexpr int hopSize = 512;
constexpr int spectrumBins = fftSize / 2;
constexpr int bandCount = 64;

constexpr float pi = 3.14159265358979323846F;

// The spectrum is carved between these two, log-spaced. Below 30 Hz is mostly
// DC offset and room rumble, and above 16 kHz most material is empty.
constexpr float bandLowHz = 30.0F;
constexpr float bandHighHz = 16000.0F;

// Everything the render side sees. Copied whole under the mutex, so a frame is
// always internally consistent rather than half of one analysis and half of the
// next.
struct Frame {
    std::array<float, bandCount> bands{};

    float bass = 0.0F;
    float lowMid = 0.0F;
    float mid = 0.0F;
    float treble = 0.0F;

    float rms = 0.0F;
    float loudness = 0.0F;

    float flux = 0.0F;
    float fluxThreshold = 0.0F;

    // Spectral centroid, 0 at 80 Hz and 1 at 6 kHz on a log scale. The latched
    // one is the value at the instant of the last onset, which is what tells a
    // kick from a hat: the continuous value has already moved on by the time
    // anything draws.
    float centroid = 0.0F;
    float beatCentroid = 0.0F;

    // Counters rather than flags. The render thread runs at its own rate and
    // compares against the count it last saw, so a beat between two frames is
    // still noticed and two beats in one frame are not collapsed into one.
    std::uint64_t beatCount = 0;
    float beatStrength = 0.0F;
    std::uint64_t kickCount = 0;
    float kickStrength = 0.0F;

    float bpm = 0.0F;
    double captureSeconds = 0.0;
    bool silent = true;
};

// Iterative radix-2, with the bit reversal and twiddles built once. 2048 points
// every 512 samples is about 94 transforms a second, which is nothing next to
// the capture itself.
class Fft {
public:
    Fft()
    {
        int bits = 0;
        while ((1 << bits) < fftSize) {
            ++bits;
        }
        m_reversed.resize(fftSize);
        for (int i = 0; i < fftSize; ++i) {
            int reversed = 0;
            for (int bit = 0; bit < bits; ++bit) {
                if ((i & (1 << bit)) != 0) {
                    reversed |= 1 << (bits - 1 - bit);
                }
            }
            m_reversed[i] = reversed;
        }
        m_twiddle.resize(fftSize / 2);
        for (int i = 0; i < fftSize / 2; ++i) {
            const float angle = -2.0F * pi * static_cast<float>(i) / static_cast<float>(fftSize);
            m_twiddle[i] = std::complex<float>(std::cos(angle), std::sin(angle));
        }
    }

    void forward(std::vector<std::complex<float>>& data) const
    {
        for (int i = 0; i < fftSize; ++i) {
            if (i < m_reversed[i]) {
                std::swap(data[i], data[m_reversed[i]]);
            }
        }
        for (int length = 2; length <= fftSize; length <<= 1) {
            const int half = length >> 1;
            const int stride = fftSize / length;
            for (int base = 0; base < fftSize; base += length) {
                for (int offset = 0; offset < half; ++offset) {
                    const std::complex<float> factor = m_twiddle[offset * stride];
                    const std::complex<float> even = data[base + offset];
                    const std::complex<float> odd = data[base + offset + half] * factor;
                    data[base + offset] = even + odd;
                    data[base + offset + half] = even - odd;
                }
            }
        }
    }

private:
    std::vector<int> m_reversed;
    std::vector<std::complex<float>> m_twiddle;
};

class Analyzer {
public:
    struct Options {
        // Empty means the monitor of whatever the default sink currently is.
        std::string device;
        float sensitivity = 1.45F;
        // Decibels of range mapped onto the 0..1 a bar height uses.
        float dynamicRangeDb = 58.0F;
        // Per-octave lift, so cymbals are visible next to a kick drum without
        // the bass having to be turned down.
        float tiltDbPerOctave = 3.0F;
    };

    ~Analyzer()
    {
        if (m_stream != nullptr) {
            pa_simple_free(m_stream);
            m_stream = nullptr;
        }
    }

    // Opens the stream on the calling thread so a bad device name is an error
    // the caller can report, rather than a thread that starts and dies quietly.
    bool open(const Options& options, std::string& error)
    {
        m_options = options;
        m_deviceName = options.device.empty() ? std::string("@DEFAULT_MONITOR@") : options.device;

        pa_sample_spec spec{};
        spec.format = PA_SAMPLE_FLOAT32NE;
        spec.channels = 2;
        spec.rate = sampleRate;

        pa_buffer_attr attr{};
        attr.maxlength = static_cast<std::uint32_t>(-1);
        attr.tlength = static_cast<std::uint32_t>(-1);
        attr.prebuf = static_cast<std::uint32_t>(-1);
        attr.minreq = static_cast<std::uint32_t>(-1);
        // One hop per read keeps capture latency near 11 ms and gives the stop
        // flag a chance to be seen that often.
        attr.fragsize = static_cast<std::uint32_t>(hopSize * 2 * sizeof(float));

        int paError = 0;
        m_stream = pa_simple_new(nullptr,
                                 "qt-runtime-cube",
                                 PA_STREAM_RECORD,
                                 m_deviceName.c_str(),
                                 "beat visualizer",
                                 &spec,
                                 nullptr,
                                 &attr,
                                 &paError);
        if (m_stream == nullptr) {
            error = std::string("could not open ") + m_deviceName + ": " + pa_strerror(paError);
            return false;
        }
        return true;
    }

    // Takes a shared_ptr to itself so the thread keeps the object alive; see the
    // note at the top about why nothing joins.
    static void spawn(const std::shared_ptr<Analyzer>& analyzer)
    {
        std::thread([analyzer] { analyzer->captureLoop(); }).detach();
    }

    void requestStop() { m_stop.store(true, std::memory_order_release); }
    [[nodiscard]] bool running() const { return m_running.load(std::memory_order_acquire); }
    [[nodiscard]] std::string device() const { return m_deviceName; }
    [[nodiscard]] std::string lastError() const
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        return m_lastError;
    }
    [[nodiscard]] std::uint64_t hopsAnalyzed() const
    {
        return m_hops.load(std::memory_order_relaxed);
    }

    [[nodiscard]] Frame snapshot() const
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        return m_frame;
    }

private:
    void captureLoop()
    {
        m_running.store(true, std::memory_order_release);
        prepare();

        std::vector<float> interleaved(static_cast<std::size_t>(hopSize) * 2);
        while (!m_stop.load(std::memory_order_acquire)) {
            int paError = 0;
            const int read = pa_simple_read(m_stream,
                                            interleaved.data(),
                                            interleaved.size() * sizeof(float),
                                            &paError);
            if (read < 0) {
                std::lock_guard<std::mutex> guard(m_mutex);
                m_lastError = std::string("capture ended: ") + pa_strerror(paError);
                break;
            }
            consume(interleaved);
        }

        m_running.store(false, std::memory_order_release);
        if (m_stream != nullptr) {
            pa_simple_free(m_stream);
            m_stream = nullptr;
        }
    }

    void prepare()
    {
        m_window.resize(fftSize);
        for (int i = 0; i < fftSize; ++i) {
            // Hann. Periodic rather than symmetric, which is the right one for
            // overlapping analysis.
            m_window[i] = 0.5F
                - 0.5F * std::cos(2.0F * pi * static_cast<float>(i) / static_cast<float>(fftSize));
        }
        m_ring.assign(fftSize, 0.0F);
        m_magnitude.assign(spectrumBins, 0.0F);
        m_previousMagnitude.assign(spectrumBins, 0.0F);
        m_scratch.resize(fftSize);

        // Log-spaced band edges, in bins.
        m_bandEdge.resize(bandCount + 1);
        const float ratio = bandHighHz / bandLowHz;
        for (int band = 0; band <= bandCount; ++band) {
            const float fraction = static_cast<float>(band) / static_cast<float>(bandCount);
            const float hz = bandLowHz * std::pow(ratio, fraction);
            const int bin = static_cast<int>(hz * static_cast<float>(fftSize)
                                             / static_cast<float>(sampleRate));
            m_bandEdge[band] = std::clamp(bin, 0, spectrumBins - 1);
        }
        // A log scale puts several of the lowest bands inside one bin, which
        // would leave them permanently identical. Push each edge past the last.
        for (int band = 1; band <= bandCount; ++band) {
            m_bandEdge[band] = std::max(m_bandEdge[band], m_bandEdge[band - 1] + 1);
            m_bandEdge[band] = std::min(m_bandEdge[band], spectrumBins - 1);
        }

        m_bandTiltDb.resize(bandCount);
        for (int band = 0; band < bandCount; ++band) {
            const float centreBin = 0.5F
                * static_cast<float>(m_bandEdge[band] + m_bandEdge[band + 1]);
            const float hz = std::max(centreBin, 1.0F) * static_cast<float>(sampleRate)
                / static_cast<float>(fftSize);
            m_bandTiltDb[band] = m_options.tiltDbPerOctave * std::log2(std::max(hz, 20.0F) / 100.0F);
        }

        m_smoothed.fill(0.0F);
    }

    void consume(const std::vector<float>& interleaved)
    {
        // Downmix and slide the ring forward by exactly one hop.
        std::rotate(m_ring.begin(), m_ring.begin() + hopSize, m_ring.end());
        float sumSquares = 0.0F;
        for (int i = 0; i < hopSize; ++i) {
            const float mono = 0.5F
                * (interleaved[static_cast<std::size_t>(i) * 2]
                   + interleaved[static_cast<std::size_t>(i) * 2 + 1]);
            m_ring[fftSize - hopSize + i] = mono;
            sumSquares += mono * mono;
        }
        const float rms = std::sqrt(sumSquares / static_cast<float>(hopSize));

        for (int i = 0; i < fftSize; ++i) {
            m_scratch[i] = std::complex<float>(m_ring[i] * m_window[i], 0.0F);
        }
        m_fft.forward(m_scratch);

        m_previousMagnitude.swap(m_magnitude);
        const float normalize = 2.0F / static_cast<float>(fftSize);
        for (int bin = 0; bin < spectrumBins; ++bin) {
            m_magnitude[bin] = std::abs(m_scratch[bin]) * normalize;
        }

        // Below about -84 dBFS there is nothing to look at, and letting the
        // adaptive ceiling chase that noise would turn silence into a full
        // display of it.
        const bool silent = rms < 6.0e-5F;

        Frame frame;
        frame.rms = rms;
        frame.silent = silent;

        computeBands(silent, frame);
        computeOnsets(silent, rms, frame);

        frame.centroid = m_centroid;
        frame.beatCentroid = m_beatCentroid;
        frame.captureSeconds = m_seconds;
        frame.bpm = m_bpm;
        frame.beatCount = m_beatCount;
        frame.beatStrength = m_beatStrength;
        frame.kickCount = m_kickCount;
        frame.kickStrength = m_kickStrength;

        m_seconds += static_cast<double>(hopSize) / static_cast<double>(sampleRate);
        m_hops.fetch_add(1, std::memory_order_relaxed);

        std::lock_guard<std::mutex> guard(m_mutex);
        m_frame = frame;
    }

    void computeBands(const bool silent, Frame& frame)
    {
        std::array<float, bandCount> raw{};
        float loudest = -160.0F;
        for (int band = 0; band < bandCount; ++band) {
            float peak = 0.0F;
            for (int bin = m_bandEdge[band]; bin < m_bandEdge[band + 1]; ++bin) {
                peak = std::max(peak, m_magnitude[bin]);
            }
            const float db = 20.0F * std::log10(peak + 1.0e-9F) + m_bandTiltDb[band];
            raw[band] = db;
            loudest = std::max(loudest, db);
        }

        // The ceiling rises immediately and falls slowly, so a track that gets
        // quieter keeps its shape for a moment instead of the whole display
        // jumping to compensate. Silence lets it sink to the floor.
        if (silent) {
            m_ceilingDb += (-70.0F - m_ceilingDb) * 0.05F;
        } else if (loudest > m_ceilingDb) {
            m_ceilingDb = loudest;
        } else {
            m_ceilingDb += (loudest - m_ceilingDb) * 0.015F;
        }

        const float floorDb = m_ceilingDb - m_options.dynamicRangeDb;
        for (int band = 0; band < bandCount; ++band) {
            float level = silent
                ? 0.0F
                : (raw[band] - floorDb) / std::max(m_options.dynamicRangeDb, 1.0F);
            level = std::clamp(level, 0.0F, 1.0F);
            // Fast up, slow down. A bar that fell as fast as it rose would read
            // as flicker rather than as a hit.
            const float rate = level > m_smoothed[band] ? 0.55F : 0.12F;
            m_smoothed[band] += (level - m_smoothed[band]) * rate;
            frame.bands[band] = m_smoothed[band];
        }

        const auto average = [&frame](const int from, const int to) {
            float total = 0.0F;
            for (int band = from; band < to; ++band) {
                total += frame.bands[band];
            }
            return total / static_cast<float>(std::max(to - from, 1));
        };
        // Roughly 30-140 Hz, 140-700 Hz, 700 Hz-4 kHz, 4-16 kHz on a 64-band
        // log carve.
        frame.bass = average(0, 10);
        frame.lowMid = average(10, 25);
        frame.mid = average(25, 45);
        frame.treble = average(45, bandCount);

        m_loudness += (average(0, bandCount) - m_loudness) * 0.15F;
        frame.loudness = m_loudness;
    }

    void computeOnsets(const bool silent, const float rms, Frame& frame)
    {
        // Where the energy sits, as one number. Weighting by magnitude over a
        // log frequency axis keeps a kick near 0 and a hi-hat near 1 without
        // either having to be loud.
        const int centroidLimit = binForHz(10000.0F);
        float weighted = 0.0F;
        float total = 0.0F;
        for (int bin = 1; bin < centroidLimit; ++bin) {
            const float hz = static_cast<float>(bin) * static_cast<float>(sampleRate)
                / static_cast<float>(fftSize);
            weighted += hz * m_magnitude[bin];
            total += m_magnitude[bin];
        }
        if (total > 1.0e-9F) {
            const float hz = std::clamp(weighted / total, 80.0F, 6000.0F);
            m_centroid = std::log2(hz / 80.0F) / std::log2(6000.0F / 80.0F);
        }

        // Spectral flux: only the bins that grew count, which is what separates
        // a note starting from a note continuing.
        const int fluxLimit = binForHz(8000.0F);
        float flux = 0.0F;
        for (int bin = 1; bin < fluxLimit; ++bin) {
            flux += std::max(0.0F, m_magnitude[bin] - m_previousMagnitude[bin]);
        }
        const int kickLimit = binForHz(170.0F);
        float kickFlux = 0.0F;
        for (int bin = binForHz(30.0F); bin < kickLimit; ++bin) {
            kickFlux += std::max(0.0F, m_magnitude[bin] - m_previousMagnitude[bin]);
        }

        frame.flux = flux;

        // About 1.5 s of history. At half that, a strong beat inflates the mean
        // it is measured against and suppresses the next half second, which is a
        // second refractory period nobody asked for and drops real off-beats.
        m_fluxHistory.push_back(flux);
        if (m_fluxHistory.size() > 144) {
            m_fluxHistory.pop_front();
        }
        m_kickHistory.push_back(kickFlux);
        if (m_kickHistory.size() > 144) {
            m_kickHistory.pop_front();
        }

        const auto mean = [](const std::deque<float>& values) {
            if (values.empty()) {
                return 0.0F;
            }
            float total = 0.0F;
            for (const float value : values) {
                total += value;
            }
            return total / static_cast<float>(values.size());
        };

        const float threshold = mean(m_fluxHistory) * m_options.sensitivity + 1.0e-5F;
        frame.fluxThreshold = threshold;

        const double refractory = 0.105;
        if (!silent && rms > 2.0e-4F && flux > threshold
            && m_seconds - m_lastBeatSeconds > refractory && m_fluxHistory.size() > 8) {
            registerBeat(flux, threshold);
        }

        const float kickThreshold = mean(m_kickHistory) * (m_options.sensitivity + 0.35F) + 1.0e-5F;
        if (!silent && kickFlux > kickThreshold && m_seconds - m_lastKickSeconds > 0.12
            && m_kickHistory.size() > 8) {
            m_lastKickSeconds = m_seconds;
            ++m_kickCount;
            m_kickStrength = std::clamp(kickFlux / std::max(kickThreshold, 1.0e-6F) - 1.0F,
                                        0.0F, 3.0F);
        }
    }

    void registerBeat(const float flux, const float threshold)
    {
        const double interval = m_seconds - m_lastBeatSeconds;
        m_lastBeatSeconds = m_seconds;
        ++m_beatCount;
        m_beatStrength = std::clamp(flux / std::max(threshold, 1.0e-6F) - 1.0F, 0.0F, 3.0F);
        m_beatCentroid = m_centroid;

        // Tempo from the median inter-beat interval, which shrugs off the
        // occasional missed or doubled beat in a way a mean does not.
        if (interval > 0.18 && interval < 2.0) {
            m_intervals.push_back(interval);
            if (m_intervals.size() > 16) {
                m_intervals.pop_front();
            }
            if (m_intervals.size() >= 5) {
                std::vector<double> sorted(m_intervals.begin(), m_intervals.end());
                std::sort(sorted.begin(), sorted.end());
                double median = sorted[sorted.size() / 2];
                // Fold into a range a listener would actually tap.
                while (median > 0.75) {
                    median *= 0.5;
                }
                while (median < 0.32) {
                    median *= 2.0;
                }
                m_bpm = static_cast<float>(60.0 / median);
            }
        }
    }

    [[nodiscard]] static int binForHz(const float hz)
    {
        const int bin = static_cast<int>(hz * static_cast<float>(fftSize)
                                         / static_cast<float>(sampleRate));
        return std::clamp(bin, 1, spectrumBins - 1);
    }

    Options m_options;
    std::string m_deviceName;
    pa_simple* m_stream = nullptr;

    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_running{false};
    std::atomic<std::uint64_t> m_hops{0};

    mutable std::mutex m_mutex;
    Frame m_frame;
    std::string m_lastError;

    Fft m_fft;
    std::vector<float> m_window;
    std::vector<float> m_ring;
    std::vector<std::complex<float>> m_scratch;
    std::vector<float> m_magnitude;
    std::vector<float> m_previousMagnitude;
    std::vector<int> m_bandEdge;
    std::vector<float> m_bandTiltDb;
    std::array<float, bandCount> m_smoothed{};

    float m_ceilingDb = -30.0F;
    float m_loudness = 0.0F;
    float m_centroid = 0.0F;
    float m_beatCentroid = 0.0F;

    std::deque<float> m_fluxHistory;
    std::deque<float> m_kickHistory;
    std::deque<double> m_intervals;

    double m_seconds = 0.0;
    double m_lastBeatSeconds = -1.0;
    double m_lastKickSeconds = -1.0;
    std::uint64_t m_beatCount = 0;
    std::uint64_t m_kickCount = 0;
    float m_beatStrength = 0.0F;
    float m_kickStrength = 0.0F;
    float m_bpm = 0.0F;
};

} // namespace audio_analysis
