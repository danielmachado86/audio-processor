#include <alsa/asoundlib.h>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <atomic>
#include <memory>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <chrono>

class DelayLine
{
private:
    std::vector<float> buffer;
    size_t maxLen;
    size_t writeIndex; // Current write position
    mutable std::mutex mutex;

public:
    explicit DelayLine(size_t cap) : maxLen(cap), writeIndex(0)
    {
        buffer.resize(maxLen);
        std::fill(buffer.begin(), buffer.end(), 0.0f);
    }

    void write(const float &sample)
    {
        std::lock_guard<std::mutex> lock(mutex);
        buffer[writeIndex] = sample;
        writeIndex = (writeIndex + 1) % maxLen;
    }

    float read(size_t delayInSamples)
    {
        std::lock_guard<std::mutex> lock(mutex);

        // Ensure delay is within bounds
        if (delayInSamples >= maxLen)
        {
            delayInSamples = maxLen - 1;
        }
        size_t readIndex = (writeIndex - delayInSamples + maxLen) % maxLen;

        return buffer[readIndex];
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
    }

    size_t getCapacity() const { return maxLen; }
};

class BatchCircularBuffer
{
private:
    std::vector<int32_t> buffer;
    size_t capacity;
    size_t head;
    size_t tail;
    std::atomic<size_t> size;
    mutable std::mutex mutex;
    std::condition_variable notEmpty;
    std::condition_variable notFull;

public:
    explicit BatchCircularBuffer(size_t cap) : capacity(cap), head(0), tail(0), size(0)
    {
        buffer.resize(capacity);
    }

    bool write(const int32_t *data, size_t length, bool blocking = true)
    {
        std::unique_lock<std::mutex> lock(mutex);

        if (!blocking && size.load() + length > capacity)
        {
            return false; // Buffer would overflow
        }

        if (blocking)
        {
            notFull.wait(lock, [this, length]
                         { return size.load() + length <= capacity; });
        }

        for (size_t i = 0; i < length; ++i)
        {
            buffer[head] = data[i];
            head = (head + 1) % capacity;
        }

        size.fetch_add(length);
        notEmpty.notify_one();
        return true;
    }

    bool read(int32_t *data, size_t length, bool blocking = true)
    {
        std::unique_lock<std::mutex> lock(mutex);

        if (!blocking && size.load() < length)
        {
            return false; // Not enough data
        }

        if (blocking)
        {
            notEmpty.wait(lock, [this, length]
                          { return size.load() >= length; });
        }

        for (size_t i = 0; i < length; ++i)
        {
            data[i] = buffer[tail];
            tail = (tail + 1) % capacity;
        }

        size.fetch_sub(length);
        notFull.notify_one();
        return true;
    }

    size_t availableForWrite() const
    {
        return capacity - size.load();
    }

    size_t availableForRead() const
    {
        return size.load();
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex);
        head = tail = 0;
        size.store(0);
        notFull.notify_all();
    }
};

class ALSADevice
{
private:
    snd_pcm_t *handle;
    std::string deviceName;
    snd_pcm_stream_t streamType;

public:
    ALSADevice() : handle(nullptr), deviceName(""), streamType(SND_PCM_STREAM_PLAYBACK) {}

    ~ALSADevice()
    {
        close();
    }

    bool open(const std::string &device, snd_pcm_stream_t stream)
    {
        deviceName = device;
        streamType = stream;

        int err = snd_pcm_open(&handle, device.c_str(), stream, 0);
        if (err < 0)
        {
            std::cerr << "Error opening PCM device " << device << ": "
                      << snd_strerror(err) << std::endl;
            return false;
        }

        return true;
    }

    bool configure(unsigned int sampleRate, unsigned int channels,
                   snd_pcm_format_t format, snd_pcm_uframes_t bufferSize,
                   snd_pcm_uframes_t periodSize)
    {
        if (!handle)
            return false;

        snd_pcm_hw_params_t *hwParams;
        snd_pcm_hw_params_alloca(&hwParams);

        // Get current hardware parameters
        int err = snd_pcm_hw_params_any(handle, hwParams);
        if (err < 0)
        {
            std::cerr << "Error getting hw params: " << snd_strerror(err) << std::endl;
            return false;
        }

        // Set access type
        err = snd_pcm_hw_params_set_access(handle, hwParams, SND_PCM_ACCESS_RW_INTERLEAVED);
        if (err < 0)
        {
            std::cerr << "Error setting access: " << snd_strerror(err) << std::endl;
            return false;
        }

        // Set sample format
        err = snd_pcm_hw_params_set_format(handle, hwParams, format);
        if (err < 0)
        {
            std::cerr << "Error setting format: " << snd_strerror(err) << std::endl;
            return false;
        }

        // Set sample rate
        unsigned int actualRate = sampleRate;
        err = snd_pcm_hw_params_set_rate_near(handle, hwParams, &actualRate, 0);
        if (err < 0)
        {
            std::cerr << "Error setting rate: " << snd_strerror(err) << std::endl;
            return false;
        }

        if (actualRate != sampleRate)
        {
            std::cout << "Requested rate " << sampleRate << " Hz, got "
                      << actualRate << " Hz" << std::endl;
        }

        // Set channels
        err = snd_pcm_hw_params_set_channels(handle, hwParams, channels);
        if (err < 0)
        {
            std::cerr << "Error setting channels: " << snd_strerror(err) << std::endl;
            return false;
        }

        // Set buffer size
        snd_pcm_uframes_t actualBufferSize = bufferSize;
        err = snd_pcm_hw_params_set_buffer_size_near(handle, hwParams, &actualBufferSize);
        if (err < 0)
        {
            std::cerr << "Error setting buffer size: " << snd_strerror(err) << std::endl;
            return false;
        }

        // Set period size
        snd_pcm_uframes_t actualPeriodSize = periodSize;
        err = snd_pcm_hw_params_set_period_size_near(handle, hwParams, &actualPeriodSize, 0);
        if (err < 0)
        {
            std::cerr << "Error setting period size: " << snd_strerror(err) << std::endl;
            return false;
        }

        // Apply hardware parameters
        err = snd_pcm_hw_params(handle, hwParams);
        if (err < 0)
        {
            std::cerr << "Error setting hw params: " << snd_strerror(err) << std::endl;
            return false;
        }

        // Configure software parameters
        snd_pcm_sw_params_t *swParams;
        snd_pcm_sw_params_alloca(&swParams);

        err = snd_pcm_sw_params_current(handle, swParams);
        if (err < 0)
        {
            std::cerr << "Error getting sw params: " << snd_strerror(err) << std::endl;
            return false;
        }

        // Start threshold
        if (streamType == SND_PCM_STREAM_PLAYBACK)
        {
            err = snd_pcm_sw_params_set_start_threshold(handle, swParams, actualPeriodSize);
        }
        else
        {
            err = snd_pcm_sw_params_set_start_threshold(handle, swParams, 1);
        }

        if (err < 0)
        {
            std::cerr << "Error setting start threshold: " << snd_strerror(err) << std::endl;
            return false;
        }

        // Apply software parameters
        err = snd_pcm_sw_params(handle, swParams);
        if (err < 0)
        {
            std::cerr << "Error setting sw params: " << snd_strerror(err) << std::endl;
            return false;
        }

        std::cout << "Device " << deviceName << " configured successfully:" << std::endl;
        std::cout << "  Sample rate: " << actualRate << " Hz" << std::endl;
        std::cout << "  Channels: " << channels << std::endl;
        std::cout << "  Buffer size: " << actualBufferSize << " frames" << std::endl;
        std::cout << "  Period size: " << actualPeriodSize << " frames" << std::endl;

        return true;
    }

    snd_pcm_sframes_t read(void *buffer, snd_pcm_uframes_t frames)
    {
        if (!handle)
            return -1;
        return snd_pcm_readi(handle, buffer, frames);
    }

    snd_pcm_sframes_t write(const void *buffer, snd_pcm_uframes_t frames)
    {
        if (!handle)
            return -1;
        return snd_pcm_writei(handle, buffer, frames);
    }

    bool prepare()
    {
        if (!handle)
            return false;
        int err = snd_pcm_prepare(handle);
        if (err < 0)
        {
            std::cerr << "Error preparing PCM: " << snd_strerror(err) << std::endl;
            return false;
        }
        return true;
    }

    bool start()
    {
        if (!handle)
            return false;
        int err = snd_pcm_start(handle);
        if (err < 0 && err != -EBADFD)
        { // -EBADFD means already started
            std::cerr << "Error starting PCM: " << snd_strerror(err) << std::endl;
            return false;
        }
        return true;
    }

    bool drop()
    {
        if (!handle)
            return false;
        int err = snd_pcm_drop(handle);
        if (err < 0)
        {
            std::cerr << "Error dropping PCM: " << snd_strerror(err) << std::endl;
            return false;
        }
        return true;
    }

    void close()
    {
        if (handle)
        {
            snd_pcm_close(handle);
            handle = nullptr;
        }
    }

    snd_pcm_state_t getState() const
    {
        return handle ? snd_pcm_state(handle) : SND_PCM_STATE_DISCONNECTED;
    }

    bool recover(int err)
    {
        if (!handle)
            return false;

        std::cout << "Recovering from error: " << snd_strerror(err) << std::endl;

        int recovery = snd_pcm_recover(handle, err, 1);
        if (recovery < 0)
        {
            std::cerr << "Recovery failed: " << snd_strerror(recovery) << std::endl;
            return false;
        }

        return prepare();
    }

    snd_pcm_t *getHandle() const { return handle; }
};

// Base class for all audio effects
class AudioEffect
{
public:
    virtual ~AudioEffect() = default;

    // Process audio samples
    // inputBuffer: input audio data
    // outputBuffer: output audio data (can be same as input for in-place processing)
    // numSamples: number of samples to process
    // channels: number of audio channels
    virtual void process(const int32_t *inputBuffer, int32_t *outputBuffer,
                         size_t numSamples, unsigned int channels) = 0;

    // Reset effect state (clear buffers, etc.)
    virtual void reset() = 0;

    // Enable/disable the effect
    virtual void setEnabled(bool enabled) { m_enabled = enabled; }
    virtual bool isEnabled() const { return m_enabled; }

    // Set sample rate (called when audio system changes sample rate)
    virtual void setSampleRate(unsigned int sampleRate) { m_sampleRate = sampleRate; }

protected:
    bool m_enabled = true;
    unsigned int m_sampleRate = 48000;
};

#pragma once
#include <vector>
#include <memory>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <random>

// All-pass filter for reverb
class AllPassFilter
{
private:
    std::vector<float> m_buffer;
    size_t m_bufferSize;
    size_t m_writeIndex;
    float m_gain;

public:
    AllPassFilter(size_t delayInSamples, float gain = 0.7f)
        : m_bufferSize(delayInSamples), m_writeIndex(0), m_gain(gain)
    {
        m_buffer.resize(m_bufferSize, 0.0f);
    }

    float process(float input)
    {
        size_t readIndex = m_writeIndex;
        float delayed = m_buffer[readIndex];

        // All-pass filter equation: y[n] = -g*x[n] + x[n-d] + g*y[n-d]
        float output = -m_gain * input + delayed;
        m_buffer[m_writeIndex] = input + m_gain * delayed;

        m_writeIndex = (m_writeIndex + 1) % m_bufferSize;
        return output;
    }

    void clear()
    {
        std::fill(m_buffer.begin(), m_buffer.end(), 0.0f);
        m_writeIndex = 0;
    }

    void setGain(float gain) { m_gain = std::clamp(gain, -0.99f, 0.99f); }
};

// Comb filter (feedback delay line) for reverb
class CombFilter
{
private:
    std::vector<float> m_buffer;
    size_t m_bufferSize;
    size_t m_writeIndex;
    float m_feedback;
    float m_damping;
    float m_filterState;

public:
    CombFilter(size_t delayInSamples, float feedback = 0.84f, float damping = 0.2f)
        : m_bufferSize(delayInSamples), m_writeIndex(0),
          m_feedback(feedback), m_damping(damping), m_filterState(0.0f)
    {
        m_buffer.resize(m_bufferSize, 0.0f);
    }

    float process(float input)
    {
        size_t readIndex = m_writeIndex;
        float delayed = m_buffer[readIndex];

        // One-pole lowpass filter for damping
        m_filterState = delayed * (1.0f - m_damping) + m_filterState * m_damping;

        m_buffer[m_writeIndex] = input + m_filterState * m_feedback;
        m_writeIndex = (m_writeIndex + 1) % m_bufferSize;

        return delayed;
    }

    void clear()
    {
        std::fill(m_buffer.begin(), m_buffer.end(), 0.0f);
        m_writeIndex = 0;
        m_filterState = 0.0f;
    }

    void setFeedback(float feedback) { m_feedback = std::clamp(feedback, 0.0f, 0.99f); }
    void setDamping(float damping) { m_damping = std::clamp(damping, 0.0f, 1.0f); }
};

// Early reflections generator
class EarlyReflections
{
private:
    static constexpr int NUM_TAPS = 8;
    std::vector<float> m_buffer;
    size_t m_bufferSize;
    size_t m_writeIndex;

    struct Tap
    {
        size_t delay;
        float gain;
    };

    std::array<Tap, NUM_TAPS> m_taps;

public:
    EarlyReflections(size_t sampleRate, float roomSize = 1.0f)
    {
        // Buffer size for maximum early reflection delay (50ms)
        m_bufferSize = static_cast<size_t>(sampleRate * 0.05f);
        m_buffer.resize(m_bufferSize, 0.0f);
        m_writeIndex = 0;

        setupTaps(sampleRate, roomSize);
    }

    void setupTaps(size_t sampleRate, float roomSize)
    {
        // Early reflection patterns based on room size
        float baseDelay = roomSize * 0.01f; // Base delay in seconds

        m_taps[0] = {static_cast<size_t>(baseDelay * 0.5f * sampleRate), 0.8f * roomSize};
        m_taps[1] = {static_cast<size_t>(baseDelay * 0.8f * sampleRate), 0.6f * roomSize};
        m_taps[2] = {static_cast<size_t>(baseDelay * 1.2f * sampleRate), 0.7f * roomSize};
        m_taps[3] = {static_cast<size_t>(baseDelay * 1.8f * sampleRate), 0.5f * roomSize};
        m_taps[4] = {static_cast<size_t>(baseDelay * 2.3f * sampleRate), 0.4f * roomSize};
        m_taps[5] = {static_cast<size_t>(baseDelay * 2.9f * sampleRate), 0.3f * roomSize};
        m_taps[6] = {static_cast<size_t>(baseDelay * 3.5f * sampleRate), 0.25f * roomSize};
        m_taps[7] = {static_cast<size_t>(baseDelay * 4.2f * sampleRate), 0.2f * roomSize};

        // Ensure delays don't exceed buffer size
        for (auto &tap : m_taps)
        {
            tap.delay = std::min(tap.delay, m_bufferSize - 1);
        }
    }

    float process(float input)
    {
        m_buffer[m_writeIndex] = input;

        float output = 0.0f;
        for (const auto &tap : m_taps)
        {
            size_t readIndex = (m_writeIndex + m_bufferSize - tap.delay) % m_bufferSize;
            output += m_buffer[readIndex] * tap.gain;
        }

        m_writeIndex = (m_writeIndex + 1) % m_bufferSize;
        return output * 0.125f; // Scale down (1/8 for 8 taps)
    }

    void clear()
    {
        std::fill(m_buffer.begin(), m_buffer.end(), 0.0f);
        m_writeIndex = 0;
    }

    void setRoomSize(float roomSize, size_t sampleRate)
    {
        setupTaps(sampleRate, roomSize);
    }
};

// Main reverb effect class
class ReverbEffect : public AudioEffect
{
public:
    enum RoomType
    {
        SMALL_ROOM,
        MEDIUM_ROOM,
        LARGE_HALL,
        CATHEDRAL,
        PLATE,
        SPRING,
        CUSTOM
    };

private:
    // Stereo comb filters (different delays for L/R)
    static constexpr int NUM_COMBS = 4;
    std::array<std::unique_ptr<CombFilter>, NUM_COMBS> m_combFiltersL;
    std::array<std::unique_ptr<CombFilter>, NUM_COMBS> m_combFiltersR;

    // Stereo all-pass filters
    static constexpr int NUM_ALLPASS = 3;
    std::array<std::unique_ptr<AllPassFilter>, NUM_ALLPASS> m_allPassFiltersL;
    std::array<std::unique_ptr<AllPassFilter>, NUM_ALLPASS> m_allPassFiltersR;

    // Early reflections
    std::unique_ptr<EarlyReflections> m_earlyReflectionsL;
    std::unique_ptr<EarlyReflections> m_earlyReflectionsR;

    // Parameters
    size_t m_sampleRate;
    size_t m_channels;
    float m_roomSize;
    float m_decay;
    float m_damping;
    float m_diffusion;
    float m_earlyReflectionLevel;
    RoomType m_roomType;

    // Convert int32_t to float for processing
    static constexpr float INT32_TO_FLOAT = 1.0f / 2147483648.0f;
    static constexpr float FLOAT_TO_INT32 = 2147483648.0f;

    inline float int32ToFloat(int32_t sample) const
    {
        return static_cast<float>(sample) * INT32_TO_FLOAT;
    }

    inline int32_t floatToInt32(float sample) const
    {
        sample = std::clamp(sample, -1.0f, 1.0f);
        return static_cast<int32_t>(sample * FLOAT_TO_INT32);
    }

public:
    ReverbEffect(size_t sampleRate, size_t channels, RoomType roomType = MEDIUM_ROOM)
        : m_sampleRate(sampleRate), m_channels(channels), m_roomType(roomType)
    {

        initializeParameters();
        createFilters();
    }

    void process(int32_t *samples, size_t numFrames, size_t channels) override
    {
        if (!m_enabled || channels != m_channels)
        {
            return;
        }

        for (size_t frame = 0; frame < numFrames; ++frame)
        {
            if (channels == 1)
            {
                // Mono processing
                float input = int32ToFloat(samples[frame]);
                float output = processMono(input);
                float mixed = input * (1.0f - m_mix) + output * m_mix;
                samples[frame] = floatToInt32(mixed);
            }
            else if (channels == 2)
            {
                // Stereo processing
                float inputL = int32ToFloat(samples[frame * 2]);
                float inputR = int32ToFloat(samples[frame * 2 + 1]);

                auto [outputL, outputR] = processStereo(inputL, inputR);

                float mixedL = inputL * (1.0f - m_mix) + outputL * m_mix;
                float mixedR = inputR * (1.0f - m_mix) + outputR * m_mix;

                samples[frame * 2] = floatToInt32(mixedL);
                samples[frame * 2 + 1] = floatToInt32(mixedR);
            }
        }
    }

    void reset() override
    {
        for (auto &comb : m_combFiltersL)
        {
            if (comb)
                comb->clear();
        }
        for (auto &comb : m_combFiltersR)
        {
            if (comb)
                comb->clear();
        }
        for (auto &allpass : m_allPassFiltersL)
        {
            if (allpass)
                allpass->clear();
        }
        for (auto &allpass : m_allPassFiltersR)
        {
            if (allpass)
                allpass->clear();
        }
        if (m_earlyReflectionsL)
            m_earlyReflectionsL->clear();
        if (m_earlyReflectionsR)
            m_earlyReflectionsR->clear();
    }

    // Room type presets
    void setRoomType(RoomType roomType)
    {
        m_roomType = roomType;
        initializeParameters();
        createFilters();
    }

    RoomType getRoomType() const { return m_roomType; }

    // Parameter controls
    void setRoomSize(float size)
    {
        m_roomSize = std::clamp(size, 0.1f, 3.0f);
        if (m_roomType == CUSTOM)
        {
            createFilters();
        }
    }

    void setDecay(float decay)
    {
        m_decay = std::clamp(decay, 0.1f, 0.99f);
        updateCombFeedback();
    }

    void setDamping(float damping)
    {
        m_damping = std::clamp(damping, 0.0f, 1.0f);
        updateCombDamping();
    }

    void setDiffusion(float diffusion)
    {
        m_diffusion = std::clamp(diffusion, 0.0f, 1.0f);
        updateAllPassGain();
    }

    void setEarlyReflectionLevel(float level)
    {
        m_earlyReflectionLevel = std::clamp(level, 0.0f, 1.0f);
    }

    // Getters
    float getRoomSize() const { return m_roomSize; }
    float getDecay() const { return m_decay; }
    float getDamping() const { return m_damping; }
    float getDiffusion() const { return m_diffusion; }
    float getEarlyReflectionLevel() const { return m_earlyReflectionLevel; }

private:
    void initializeParameters()
    {
        switch (m_roomType)
        {
        case SMALL_ROOM:
            m_roomSize = 0.3f;
            m_decay = 0.5f;
            m_damping = 0.3f;
            m_diffusion = 0.6f;
            m_earlyReflectionLevel = 0.4f;
            break;

        case MEDIUM_ROOM:
            m_roomSize = 0.7f;
            m_decay = 0.7f;
            m_damping = 0.2f;
            m_diffusion = 0.7f;
            m_earlyReflectionLevel = 0.3f;
            break;

        case LARGE_HALL:
            m_roomSize = 1.5f;
            m_decay = 0.85f;
            m_damping = 0.15f;
            m_diffusion = 0.8f;
            m_earlyReflectionLevel = 0.2f;
            break;

        case CATHEDRAL:
            m_roomSize = 2.5f;
            m_decay = 0.92f;
            m_damping = 0.1f;
            m_diffusion = 0.9f;
            m_earlyReflectionLevel = 0.15f;
            break;

        case PLATE:
            m_roomSize = 0.8f;
            m_decay = 0.8f;
            m_damping = 0.05f;
            m_diffusion = 0.95f;
            m_earlyReflectionLevel = 0.1f;
            break;

        case SPRING:
            m_roomSize = 0.4f;
            m_decay = 0.6f;
            m_damping = 0.4f;
            m_diffusion = 0.5f;
            m_earlyReflectionLevel = 0.5f;
            break;

        case CUSTOM:
            // Keep current values
            break;
        }

        setMix(0.3f); // Default 30% wet
    }

    void createFilters()
    {
        // Comb filter delays based on room size (in samples)
        float baseDelay = m_roomSize * m_sampleRate * 0.03f; // 30ms base for room size 1.0

        // Left channel comb delays (prime numbers scaled by room size)
        std::array<float, NUM_COMBS> combDelaysL = {
            baseDelay * 1.0f,
            baseDelay * 1.13f,
            baseDelay * 1.27f,
            baseDelay * 1.41f};

        // Right channel comb delays (slightly different for stereo width)
        std::array<float, NUM_COMBS> combDelaysR = {
            baseDelay * 1.05f,
            baseDelay * 1.18f,
            baseDelay * 1.32f,
            baseDelay * 1.46f};

        // Create comb filters
        for (int i = 0; i < NUM_COMBS; ++i)
        {
            m_combFiltersL[i] = std::make_unique<CombFilter>(
                static_cast<size_t>(combDelaysL[i]), m_decay, m_damping);
            m_combFiltersR[i] = std::make_unique<CombFilter>(
                static_cast<size_t>(combDelaysR[i]), m_decay, m_damping);
        }

        // All-pass filter delays
        float allpassBase = m_roomSize * m_sampleRate * 0.005f; // 5ms base
        std::array<float, NUM_ALLPASS> allpassDelaysL = {
            allpassBase * 1.0f,
            allpassBase * 2.1f,
            allpassBase * 3.7f};

        std::array<float, NUM_ALLPASS> allpassDelaysR = {
            allpassBase * 1.1f,
            allpassBase * 2.3f,
            allpassBase * 3.9f};

        // Create all-pass filters
        for (int i = 0; i < NUM_ALLPASS; ++i)
        {
            m_allPassFiltersL[i] = std::make_unique<AllPassFilter>(
                static_cast<size_t>(allpassDelaysL[i]), m_diffusion * 0.7f);
            m_allPassFiltersR[i] = std::make_unique<AllPassFilter>(
                static_cast<size_t>(allpassDelaysR[i]), m_diffusion * 0.7f);
        }

        // Create early reflections
        m_earlyReflectionsL = std::make_unique<EarlyReflections>(m_sampleRate, m_roomSize);
        m_earlyReflectionsR = std::make_unique<EarlyReflections>(m_sampleRate, m_roomSize * 1.05f);
    }

    void updateCombFeedback()
    {
        for (auto &comb : m_combFiltersL)
        {
            if (comb)
                comb->setFeedback(m_decay);
        }
        for (auto &comb : m_combFiltersR)
        {
            if (comb)
                comb->setFeedback(m_decay);
        }
    }

    void updateCombDamping()
    {
        for (auto &comb : m_combFiltersL)
        {
            if (comb)
                comb->setDamping(m_damping);
        }
        for (auto &comb : m_combFiltersR)
        {
            if (comb)
                comb->setDamping(m_damping);
        }
    }

    void updateAllPassGain()
    {
        float gain = m_diffusion * 0.7f;
        for (auto &allpass : m_allPassFiltersL)
        {
            if (allpass)
                allpass->setGain(gain);
        }
        for (auto &allpass : m_allPassFiltersR)
        {
            if (allpass)
                allpass->setGain(gain);
        }
    }

    float processMono(float input)
    {
        // Early reflections
        float early = m_earlyReflectionsL->process(input) * m_earlyReflectionLevel;

        // Comb filters (parallel)
        float combOut = 0.0f;
        for (auto &comb : m_combFiltersL)
        {
            combOut += comb->process(input);
        }
        combOut *= 0.25f; // Scale for 4 combs

        // All-pass filters (series)
        float allpassOut = combOut;
        for (auto &allpass : m_allPassFiltersL)
        {
            allpassOut = allpass->process(allpassOut);
        }

        return early + allpassOut * 0.7f;
    }

    std::pair<float, float> processStereo(float inputL, float inputR)
    {
        // Mono sum for reverb input
        float monoInput = (inputL + inputR) * 0.5f;

        // Early reflections
        float earlyL = m_earlyReflectionsL->process(monoInput) * m_earlyReflectionLevel;
        float earlyR = m_earlyReflectionsR->process(monoInput) * m_earlyReflectionLevel;

        // Comb filters (parallel) - separate L/R
        float combOutL = 0.0f;
        float combOutR = 0.0f;

        for (auto &comb : m_combFiltersL)
        {
            combOutL += comb->process(monoInput);
        }
        for (auto &comb : m_combFiltersR)
        {
            combOutR += comb->process(monoInput);
        }

        combOutL *= 0.25f;
        combOutR *= 0.25f;

        // All-pass filters (series)
        float allpassOutL = combOutL;
        float allpassOutR = combOutR;

        for (auto &allpass : m_allPassFiltersL)
        {
            allpassOutL = allpass->process(allpassOutL);
        }
        for (auto &allpass : m_allPassFiltersR)
        {
            allpassOutR = allpass->process(allpassOutR);
        }

        return {earlyL + allpassOutL * 0.7f, earlyR + allpassOutR * 0.7f};
    }
};

// Delay effect implementation
class DelayEffect : public AudioEffect
{
private:
    std::vector<std::vector<int32_t>> m_delayBuffers; // One buffer per channel
    std::vector<size_t> m_writeIndices;               // Write position for each channel
    size_t m_bufferSize;
    size_t m_delaySamples;
    float m_feedback;
    float m_wetLevel;
    float m_dryLevel;

public:
    DelayEffect(float delayTimeMs = 250.0f, float feedback = 0.3f,
                float wetLevel = 0.3f, float dryLevel = 0.7f)
        : m_feedback(feedback), m_wetLevel(wetLevel), m_dryLevel(dryLevel)
    {
        setDelayTime(delayTimeMs);
    }

    void setDelayTime(float delayTimeMs)
    {
        m_delaySamples = static_cast<size_t>((delayTimeMs / 1000.0f) * m_sampleRate);
        // Add some extra buffer space to prevent overflow
        m_bufferSize = m_delaySamples + 1024;
        reset();
    }

    void setFeedback(float feedback)
    {
        // Prevent runaway feedback
        m_feedback = std::max(0.0f, std::min(0.95f, feedback));
    }

    void setWetLevel(float wetLevel)
    {
        m_wetLevel = std::max(0.0f, std::min(1.0f, wetLevel));
    }

    void setDryLevel(float dryLevel)
    {
        m_dryLevel = std::max(0.0f, std::min(1.0f, dryLevel));
    }

    void setMix(float wetLevel, float dryLevel)
    {
        setWetLevel(wetLevel);
        setDryLevel(dryLevel);
    }

    // Getters
    float getDelayTimeMs() const
    {
        return (static_cast<float>(m_delaySamples) / m_sampleRate) * 1000.0f;
    }

    float getFeedback() const { return m_feedback; }
    float getWetLevel() const { return m_wetLevel; }
    float getDryLevel() const { return m_dryLevel; }

    void setSampleRate(unsigned int sampleRate) override
    {
        float currentDelayMs = getDelayTimeMs();
        AudioEffect::setSampleRate(sampleRate);
        setDelayTime(currentDelayMs); // Recalculate delay samples for new sample rate
    }

    void reset() override
    {
        // Initialize delay buffers for each channel
        const size_t maxChannels = 8; // Support up to 8 channels
        m_delayBuffers.resize(maxChannels);
        m_writeIndices.resize(maxChannels);

        for (auto &buffer : m_delayBuffers)
        {
            buffer.clear();
            buffer.resize(m_bufferSize, 0);
        }

        std::fill(m_writeIndices.begin(), m_writeIndices.end(), 0);
    }

    void process(const int32_t *inputBuffer, int32_t *outputBuffer,
                 size_t numSamples, unsigned int channels) override
    {
        if (!m_enabled || channels == 0)
        {
            // Pass through
            if (inputBuffer != outputBuffer)
            {
                std::memcpy(outputBuffer, inputBuffer, numSamples * channels * sizeof(int32_t));
            }
            return;
        }

        // Ensure we have enough delay buffers
        if (m_delayBuffers.size() < channels)
        {
            m_delayBuffers.resize(channels);
            m_writeIndices.resize(channels);
            for (size_t i = 0; i < channels; ++i)
            {
                if (m_delayBuffers[i].size() != m_bufferSize)
                {
                    m_delayBuffers[i].clear();
                    m_delayBuffers[i].resize(m_bufferSize, 0);
                    m_writeIndices[i] = 0;
                }
            }
        }

        for (size_t sample = 0; sample < numSamples; ++sample)
        {
            for (unsigned int ch = 0; ch < channels; ++ch)
            {
                const size_t bufferIndex = sample * channels + ch;
                const int32_t inputSample = inputBuffer[bufferIndex];

                // Calculate read position (delay samples behind write position)
                const size_t readIndex = (m_writeIndices[ch] + m_bufferSize - m_delaySamples) % m_bufferSize;

                // Get delayed sample
                const int32_t delayedSample = m_delayBuffers[ch][readIndex];

                // Calculate feedback sample (delayed sample * feedback)
                const int64_t feedbackSample = static_cast<int64_t>(delayedSample * m_feedback);

                // Write to delay buffer (input + feedback)
                const int64_t bufferInput = static_cast<int64_t>(inputSample) + feedbackSample;

                // Clamp to prevent overflow
                m_delayBuffers[ch][m_writeIndices[ch]] = static_cast<int32_t>(
                    std::max(static_cast<int64_t>(INT32_MIN), std::min(static_cast<int64_t>(INT32_MAX), bufferInput)));

                // Mix dry and wet signals
                const int64_t drySignal = static_cast<int64_t>(inputSample * m_dryLevel);
                const int64_t wetSignal = static_cast<int64_t>(delayedSample * m_wetLevel);
                const int64_t mixedSignal = drySignal + wetSignal;

                // Clamp and store output
                outputBuffer[bufferIndex] = static_cast<int32_t>(
                    std::max(static_cast<int64_t>(INT32_MIN), std::min(static_cast<int64_t>(INT32_MAX), mixedSignal)));

                // Advance write position
                m_writeIndices[ch] = (m_writeIndices[ch] + 1) % m_bufferSize;
            }
        }
    }
};

// Effect chain manager
class AudioEffectChain
{
private:
    std::vector<std::unique_ptr<AudioEffect>> m_effects;
    std::vector<int32_t> m_tempBuffer;

public:
    void addEffect(std::unique_ptr<AudioEffect> effect)
    {
        m_effects.push_back(std::move(effect));
    }

    void removeEffect(size_t index)
    {
        if (index < m_effects.size())
        {
            m_effects.erase(m_effects.begin() + index);
        }
    }

    void clearEffects()
    {
        m_effects.clear();
    }

    AudioEffect *getEffect(size_t index)
    {
        return (index < m_effects.size()) ? m_effects[index].get() : nullptr;
    }

    size_t getEffectCount() const
    {
        return m_effects.size();
    }

    void setSampleRate(unsigned int sampleRate)
    {
        for (auto &effect : m_effects)
        {
            effect->setSampleRate(sampleRate);
        }
    }

    void reset()
    {
        for (auto &effect : m_effects)
        {
            effect->reset();
        }
    }

    void process(const int32_t *inputBuffer, int32_t *outputBuffer,
                 size_t numSamples, unsigned int channels)
    {
        if (m_effects.empty())
        {
            // No effects, just copy input to output
            if (inputBuffer != outputBuffer)
            {
                std::memcpy(outputBuffer, inputBuffer, numSamples * channels * sizeof(int32_t));
            }
            return;
        }

        // Ensure temp buffer is large enough
        const size_t totalSamples = numSamples * channels;
        if (m_tempBuffer.size() < totalSamples)
        {
            m_tempBuffer.resize(totalSamples);
        }

        // Process through effect chain
        const int32_t *currentInput = inputBuffer;
        int32_t *currentOutput = (m_effects.size() == 1) ? outputBuffer : m_tempBuffer.data();

        for (size_t i = 0; i < m_effects.size(); ++i)
        {
            // For the last effect, output directly to the final output buffer
            if (i == m_effects.size() - 1)
            {
                currentOutput = outputBuffer;
            }

            m_effects[i]->process(currentInput, currentOutput, numSamples, channels);

            // For next iteration, current output becomes input
            if (i < m_effects.size() - 1)
            {
                currentInput = currentOutput;
                // Alternate between temp buffer and output buffer for ping-pong processing
                currentOutput = (currentInput == m_tempBuffer.data()) ? outputBuffer : m_tempBuffer.data();
            }
        }
    }
};

class AudioProcessor
{
private:
    ALSADevice captureDevice;
    ALSADevice playbackDevice;
    std::unique_ptr<BatchCircularBuffer> firstBuffer;
    std::unique_ptr<BatchCircularBuffer> secondBuffer;
    std::vector<std::unique_ptr<DelayLine>> delayBuffers;

    std::atomic<bool>
        running;
    std::thread captureThread;
    std::thread processingThread;
    std::thread playbackThread;

    AudioEffectChain m_effectChain;
    std::unique_ptr<ReverbEffect> m_reverbEffect;
    std::unique_ptr<DelayEffect> m_delayEffect;

public:
    // Audio parameters
    static constexpr unsigned int SAMPLE_RATE = 48000;
    static constexpr unsigned int CHANNELS = 2;
    static constexpr snd_pcm_format_t FORMAT = SND_PCM_FORMAT_S32_LE;
    static constexpr snd_pcm_uframes_t PERIOD_SIZE = 120;
    static constexpr snd_pcm_uframes_t BUFFER_SIZE = PERIOD_SIZE * 2;

    // Buffer parameters
    static constexpr size_t FRAME_SIZE = CHANNELS * sizeof(int32_t);
    static constexpr size_t AUDIO_BUFFER_SIZE = FRAME_SIZE * PERIOD_SIZE * 8; // ~92ms buffer

    size_t getAudioBufferSize() const
    {
        return AUDIO_BUFFER_SIZE;
    }

    size_t getSampleRate() const
    {
        return SAMPLE_RATE;
    }

    size_t getChannels() const
    {
        return CHANNELS;
    }

    AudioProcessor() : running(false)
    {
        firstBuffer = std::make_unique<BatchCircularBuffer>(getAudioBufferSize());
        secondBuffer = std::make_unique<BatchCircularBuffer>(getAudioBufferSize());
    }

    ~AudioProcessor()
    {
        stop();
    }

    bool initialize(const std::string &captureDeviceName = "default",
                    const std::string &playbackDeviceName = "default")
    {

        std::cout << "Initializing audio processor..." << std::endl;

        // Open and configure capture device
        if (!captureDevice.open(captureDeviceName, SND_PCM_STREAM_CAPTURE))
        {
            return false;
        }

        if (!captureDevice.configure(SAMPLE_RATE, CHANNELS, FORMAT, BUFFER_SIZE, PERIOD_SIZE))
        {
            return false;
        }

        // Open and configure playback device
        if (!playbackDevice.open(playbackDeviceName, SND_PCM_STREAM_PLAYBACK))
        {
            return false;
        }

        if (!playbackDevice.configure(SAMPLE_RATE, CHANNELS, FORMAT, BUFFER_SIZE, PERIOD_SIZE))
        {
            return false;
        }

        // Agregar reverb
        m_reverbEffect = std::make_unique<ReverbEffect>(SAMPLE_RATE, CHANNELS, ReverbEffect::MEDIUM_ROOM);
        m_reverbEffect->setMix(0.3f); // 30% wet
        m_effectChain.addEffect(std::move(m_reverbEffect));

        // Initialize effect chain (add this at the end of the method)
        m_delayEffect = std::make_unique<DelayEffect>(getSampleRate(), getChannels());
        m_delayEffect->setDelayTime(250.0f); // 250ms delay
        m_delayEffect->setFeedback(0.3f);    // 30% feedback
        m_delayEffect->setMix(0.4f, 0.6f);   // 40% wet signal
        m_effectChain.addEffect(std::move(m_delayEffect));

        std::cout << "Audio processor initialized successfully" << std::endl;
        return true;
    }

    bool start()
    {
        if (running.load())
        {
            std::cout << "Audio processor already running" << std::endl;
            return false;
        }

        // Prepare devices
        if (!captureDevice.prepare() || !playbackDevice.prepare())
        {
            return false;
        }

        running.store(true);

        // Start threads
        processingThread = std::thread(&AudioProcessor::processingLoop, this);
        captureThread = std::thread(&AudioProcessor::captureLoop, this);
        playbackThread = std::thread(&AudioProcessor::playbackLoop, this);

        std::cout << "Audio processing started" << std::endl;
        return true;
    }

    void stop()
    {
        if (!running.load())
            return;

        std::cout << "Stopping audio processor..." << std::endl;
        running.store(false);

        // Wake up threads
        firstBuffer->clear();
        secondBuffer->clear();

        // Wait for threads to finish
        if (captureThread.joinable())
        {
            captureThread.join();
        }
        std::cout << "Prueba de flujo..." << std::endl;
        // Wait for threads to finish
        if (processingThread.joinable())
        {
            processingThread.join();
        }

        if (playbackThread.joinable())
        {
            playbackThread.join();
        }

        // Stop and drop devices
        captureDevice.drop();
        playbackDevice.drop();

        std::cout << "Audio processor stopped" << std::endl;
    }

    void printStatus() const
    {
        std::cout << "\n=== Audio Processor Status ===" << std::endl;
        std::cout << "Running: " << (running.load() ? "Yes" : "No") << std::endl;
        std::cout << "First buffer usage: " << firstBuffer->availableForRead()
                  << " / " << getAudioBufferSize() << " bytes" << std::endl;
        std::cout << "Second buffer usage: " << secondBuffer->availableForRead()
                  << " / " << getAudioBufferSize() << " bytes" << std::endl;
        std::cout << "Capture state: " << snd_pcm_state_name(captureDevice.getState()) << std::endl;
        std::cout << "Playback state: " << snd_pcm_state_name(playbackDevice.getState()) << std::endl;
        std::cout << "===============================" << std::endl;
    }
    // Effect control methods
    void setDelayEnabled(bool enabled)
    {
        if (auto *delay = dynamic_cast<DelayEffect *>(m_effectChain.getEffect(0)))
        {
            delay->setEnabled(enabled);
        }
    }

    void setDelayTime(float delayMs)
    {
        if (auto *delay = dynamic_cast<DelayEffect *>(m_effectChain.getEffect(0)))
        {
            delay->setDelayTime(delayMs);
        }
    }

    void setDelayFeedback(float feedback)
    {
        if (auto *delay = dynamic_cast<DelayEffect *>(m_effectChain.getEffect(0)))
        {
            delay->setFeedback(feedback);
        }
    }

    void setDelayMix(float wetLevel, float dryLevel)
    {
        if (auto *delay = dynamic_cast<DelayEffect *>(m_effectChain.getEffect(0)))
        {
            delay->setMix(wetLevel, dryLevel);
        }
    }

    void resetEffects()
    {
        m_effectChain.reset();
    }

private:
    void captureLoop()
    {
        std::vector<int32_t> captureBuffer(PERIOD_SIZE * CHANNELS);

        std::cout << "Capture thread started" << std::endl;

        // Start capture device
        if (!captureDevice.start())
        {
            running.store(false);
            return;
        }

        // Pre-fill playback buffer with silence to avoid underruns
        std::fill(captureBuffer.begin(), captureBuffer.end(), 0);
        for (int i = 0; i < 5; ++i)
        {
            secondBuffer->write(captureBuffer.data(), PERIOD_SIZE * FRAME_SIZE);
        }

        while (running.load())
        {
            snd_pcm_sframes_t framesRead = captureDevice.read(captureBuffer.data(), PERIOD_SIZE);

            if (framesRead < 0)
            {
                if (framesRead == -EAGAIN)
                {
                    continue; // Try again
                }

                std::cerr << "Capture error: " << snd_strerror(framesRead) << std::endl;

                if (!captureDevice.recover(framesRead))
                {
                    std::cerr << "Failed to recover capture device" << std::endl;
                    running.store(false);
                    break;
                }
                continue;
            }

            if (framesRead != PERIOD_SIZE)
            {
                std::cout << "Capture: expected " << PERIOD_SIZE
                          << " frames, got " << framesRead << std::endl;
            }

            size_t bytesToWrite = framesRead * FRAME_SIZE;

            // Write to circular buffer
            const int32_t *data = reinterpret_cast<const int32_t *>(captureBuffer.data());
            if (!firstBuffer->write(data, bytesToWrite, false))
            {
                // Buffer overflow - skip this frame
                std::cout << "Audio buffer overflow, dropping captured frame" << std::endl;
            }
        }
        std::cout << "Capture thread finished" << std::endl;
    }

    void processingLoop()
    {
        std::vector<int32_t> processingBuffer(PERIOD_SIZE * CHANNELS);

        std::cout << "Processing thread started" << std::endl;

        while (running.load())
        {
            // Read from circular buffer
            int32_t *data = processingBuffer.data();

            if (!firstBuffer->read(data, PERIOD_SIZE * FRAME_SIZE, true))
            {
                // Not enough data available - play silence
                // std::fill(processingBuffer.begin(), processingBuffer.end(), 0);
                std::cout << "Processing buffer underrun, playing silence" << std::endl;
            }

            //
            m_effectChain.process(data, data, PERIOD_SIZE, CHANNELS);

            if (!secondBuffer->write(data, PERIOD_SIZE * FRAME_SIZE, false))
            {
                // Buffer overflow - skip this frame
                std::cout << "Processing buffer overflow, dropping captured frame" << std::endl;
            }
        }

        // for (size_t ch = 0; ch < CHANNELS; ++ch)
        // {
        //     delayBuffers[ch]->clear();
        // }
        // std::cout << "Processing thread finished" << std::endl;
    }

    void
    playbackLoop()
    {
        std::vector<int32_t> playbackBuffer(PERIOD_SIZE * CHANNELS);
        const size_t bufferSizeBytes = playbackBuffer.size() * sizeof(int32_t);

        std::cout << "Playback thread started " << std::endl;

        // Pre-fill playback buffer with silence to avoid underruns
        std::fill(playbackBuffer.begin(), playbackBuffer.end(), 0);
        for (int i = 0; i < 2; ++i)
        {
            playbackDevice.write(playbackBuffer.data(), PERIOD_SIZE);
        }

        while (running.load())
        {

            if (!secondBuffer->read(playbackBuffer.data(), bufferSizeBytes, false))
            {
                // Not enough data available - play silence
                std::fill(playbackBuffer.begin(), playbackBuffer.end(), 0);
                std::cout << "Audio buffer underrun, playing silence" << std::endl;
            }

            void *data = reinterpret_cast<void *>(playbackBuffer.data());

            snd_pcm_sframes_t framesWritten = playbackDevice.write(data, PERIOD_SIZE);

            if (framesWritten < 0)
            {
                if (framesWritten == -EAGAIN)
                {
                    continue; // Try again
                }

                std::cerr << "Playback error: " << snd_strerror(framesWritten) << std::endl;

                if (!playbackDevice.recover(framesWritten))
                {
                    std::cerr << "Failed to recover playback device" << std::endl;
                    running.store(false);
                    break;
                }
                continue;
            }

            if (framesWritten != PERIOD_SIZE)
            {
                std::cout << "Playback: expected " << PERIOD_SIZE
                          << " frames, wrote " << framesWritten << std::endl;
            }
        }

        std::cout << "Playback thread finished" << std::endl;
    }
};

int main(int argc, char *argv[])
{
    std::string captureDevice = "default";
    std::string playbackDevice = "default";

    // Parse command line arguments
    if (argc >= 2)
        captureDevice = argv[1];
    if (argc >= 3)
        playbackDevice = argv[2];

    std::cout << "ALSA Audio Processor" << std::endl;
    std::cout << "Capture device: " << captureDevice << std::endl;
    std::cout << "Playback device: " << playbackDevice << std::endl;
    std::cout << "Sample rate: " << AudioProcessor::SAMPLE_RATE << " Hz" << std::endl;
    std::cout << "Channels: " << AudioProcessor::CHANNELS << std::endl;
    std::cout << "Format: 16-bit signed little endian" << std::endl;
    std::cout << "===========================================" << std::endl;

    AudioProcessor processor;

    if (!processor.initialize(captureDevice, playbackDevice))
    {
        std::cerr << "Failed to initialize audio processor" << std::endl;
        return 1;
    }

    // Perform ALSA operations here, e.g., writing audio data

    if (!processor.start())
    {
        std::cerr << "Failed to start audio processor" << std::endl;
        return 1;
    }

    std::cout << "\nAudio processing active. Commands:" << std::endl;
    std::cout << "  's' - Show status" << std::endl;
    std::cout << "  'd' - Toggle delay effect" << std::endl;
    std::cout << "  't' - Set delay time (ms)" << std::endl;
    std::cout << "  'f' - Set feedback (0.0-0.9)" << std::endl;
    std::cout << "  'm' - Set mix (0.0-1.0)" << std::endl;
    std::cout << "  'r' - Reset effects" << std::endl;
    std::cout << "  'q' - Quit" << std::endl;
    std::cout << "Enter command: ";

    char command;
    while (std::cin >> command)
    {
        switch (command)
        {
        case 's':
            processor.printStatus();
            break;

        case 'd':
            // Toggle delay effect
            static bool delayEnabled = true;
            delayEnabled = !delayEnabled;
            processor.setDelayEnabled(delayEnabled);
            std::cout << "Delay effect " << (delayEnabled ? "enabled" : "disabled") << std::endl;
            break;

        case 't':
        {
            float delayTime;
            std::cout << "Enter delay time (1-1000ms): ";
            std::cin >> delayTime;
            processor.setDelayTime(delayTime);
            std::cout << "Delay time set to " << delayTime << "ms" << std::endl;
        }
        break;

        case 'f':
        {
            float feedback;
            std::cout << "Enter feedback (0.0-0.9): ";
            std::cin >> feedback;
            processor.setDelayFeedback(feedback);
            std::cout << "Feedback set to " << feedback << std::endl;
        }
        break;

        case 'm':
        {
            float wetLevel;
            float dryLevel;

            std::cout << "Enter wet level (0.0-1.0): ";
            std::cin >> wetLevel;
            std::cout << "Enter dry level (0.0-1.0): ";
            std::cin >> dryLevel;
            processor.setDelayMix(wetLevel, dryLevel);
            std::cout << "Wet level set to " << wetLevel << std::endl;
            std::cout << "Dry level set to " << dryLevel << std::endl;
        }
        break;

        case 'r':
            processor.resetEffects();
            std::cout << "Effects reset" << std::endl;
            break;

        case 'q':
            std::cout << "Shutting down..." << std::endl;
            processor.stop();
            return 0;

        default:
            std::cout << "Unknown command." << std::endl;
            break;
        }
        std::cout << "Enter command: ";
    }

    return 0;
}