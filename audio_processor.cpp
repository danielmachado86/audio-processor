#include <iostream>
#include <cmath>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <algorithm>
#include <alsa/asoundlib.h>

class AudioBuffer
{
private:
    std::vector<float> buffer;
    size_t size;
    size_t writeIndex;

public:
    AudioBuffer(size_t bufferSize) : size(bufferSize), writeIndex(0)
    {
        buffer.resize(size, 0.0f);
    }

    void write(float sample)
    {
        buffer[writeIndex] = sample;
        writeIndex = (writeIndex + 1) % size;
    }

    float read(size_t delaySamples) const
    {
        if (delaySamples >= size)
            delaySamples = size - 1;
        size_t readIndex = (writeIndex + size - delaySamples) % size;
        return buffer[readIndex];
    }

    float readInterpolated(float delaySamples) const
    {
        int intDelay = static_cast<int>(delaySamples);
        float fracDelay = delaySamples - intDelay;

        float sample1 = read(intDelay);
        float sample2 = read(intDelay + 1);

        return sample1 * (1.0f - fracDelay) + sample2 * fracDelay;
    }
};

class DelayEffect
{
private:
    AudioBuffer delayBuffer;
    float feedback;
    float wetLevel;
    float dryLevel;

public:
    DelayEffect(float sampleRate, float delayTimeMs, float fb = 0.3f, float wet = 0.3f)
        : delayBuffer(static_cast<size_t>(sampleRate * delayTimeMs / 1000.0f)), feedback(fb), wetLevel(wet), dryLevel(1.0f - wet) {}

    float process(float input)
    {
        float delayed = delayBuffer.read(delayBuffer.size - 1);
        delayBuffer.write(input + delayed * feedback);
        return input * dryLevel + delayed * wetLevel;
    }

    void setFeedback(float fb) { feedback = std::clamp(fb, 0.0f, 0.95f); }
    void setWetLevel(float wet)
    {
        wetLevel = std::clamp(wet, 0.0f, 1.0f);
        dryLevel = 1.0f - wetLevel;
    }
};

class FlangerEffect
{
private:
    AudioBuffer delayBuffer;
    float sampleRate;
    float lfoPhase;
    float lfoRate;
    float depth;
    float feedback;
    float wetLevel;
    float dryLevel;

public:
    FlangerEffect(float sr, float rate = 0.5f, float d = 0.7f, float fb = 0.3f, float wet = 0.5f)
        : delayBuffer(static_cast<size_t>(sr * 0.02f)) // 20ms max delay
          ,
          sampleRate(sr), lfoPhase(0.0f), lfoRate(rate), depth(d), feedback(fb), wetLevel(wet), dryLevel(1.0f - wet)
    {
    }

    float process(float input)
    {
        // LFO para modular el delay
        float lfo = std::sin(lfoPhase) * depth;
        lfoPhase += 2.0f * M_PI * lfoRate / sampleRate;
        if (lfoPhase >= 2.0f * M_PI)
            lfoPhase -= 2.0f * M_PI;

        // Delay variable entre 1-10ms
        float delayTime = 1.0f + (lfo + 1.0f) * 4.5f; // 1-10ms
        float delaySamples = delayTime * sampleRate / 1000.0f;

        float delayed = delayBuffer.readInterpolated(delaySamples);
        delayBuffer.write(input + delayed * feedback);

        return input * dryLevel + delayed * wetLevel;
    }

    void setRate(float rate) { lfoRate = std::clamp(rate, 0.1f, 5.0f); }
    void setDepth(float d) { depth = std::clamp(d, 0.0f, 1.0f); }
    void setFeedback(float fb) { feedback = std::clamp(fb, 0.0f, 0.95f); }
};

class ChorusEffect
{
private:
    std::vector<AudioBuffer> delayBuffers;
    std::vector<float> lfoPhases;
    std::vector<float> lfoRates;
    float sampleRate;
    float depth;
    float wetLevel;
    float dryLevel;
    int numVoices;

public:
    ChorusEffect(float sr, int voices = 3, float d = 0.8f, float wet = 0.4f)
        : sampleRate(sr), depth(d), wetLevel(wet), dryLevel(1.0f - wet), numVoices(voices)
    {

        // Crear múltiples líneas de delay con diferentes parámetros
        for (int i = 0; i < numVoices; ++i)
        {
            delayBuffers.emplace_back(static_cast<size_t>(sr * 0.05f)); // 50ms max
            lfoPhases.push_back(i * 2.0f * M_PI / numVoices);           // Fases desfasadas
            lfoRates.push_back(0.3f + i * 0.15f);                       // Diferentes velocidades de LFO
        }
    }

    float process(float input)
    {
        float chorusOutput = 0.0f;

        for (int i = 0; i < numVoices; ++i)
        {
            // LFO diferente para cada voz
            float lfo = std::sin(lfoPhases[i]) * depth;
            lfoPhases[i] += 2.0f * M_PI * lfoRates[i] / sampleRate;
            if (lfoPhases[i] >= 2.0f * M_PI)
                lfoPhases[i] -= 2.0f * M_PI;

            // Delay variable entre 10-40ms para cada voz
            float delayTime = 10.0f + (lfo + 1.0f) * 15.0f;
            float delaySamples = delayTime * sampleRate / 1000.0f;

            float delayed = delayBuffers[i].readInterpolated(delaySamples);
            delayBuffers[i].write(input);

            chorusOutput += delayed;
        }

        chorusOutput /= numVoices; // Promedio de las voces
        return input * dryLevel + chorusOutput * wetLevel;
    }

    void setDepth(float d) { depth = std::clamp(d, 0.0f, 1.0f); }
    void setWetLevel(float wet)
    {
        wetLevel = std::clamp(wet, 0.0f, 1.0f);
        dryLevel = 1.0f - wetLevel;
    }
};

class AudioProcessor
{
private:
    snd_pcm_t *captureHandle;
    snd_pcm_t *playbackHandle;
    std::unique_ptr<DelayEffect> delay;
    std::unique_ptr<FlangerEffect> flanger;
    std::unique_ptr<ChorusEffect> chorus;
    std::atomic<bool> running;

    static constexpr unsigned int SAMPLE_RATE = 44100;
    static constexpr unsigned int BUFFER_SIZE = 256;
    static constexpr snd_pcm_format_t FORMAT = SND_PCM_FORMAT_S16_LE;

public:
    AudioProcessor() : captureHandle(nullptr), playbackHandle(nullptr), running(false) {}

    ~AudioProcessor()
    {
        stop();
        if (captureHandle)
            snd_pcm_close(captureHandle);
        if (playbackHandle)
            snd_pcm_close(playbackHandle);
    }

    bool initialize()
    {
        // Configurar captura de audio
        int err = snd_pcm_open(&captureHandle, "default", SND_PCM_STREAM_CAPTURE, 0);
        if (err < 0)
        {
            std::cerr << "Error abriendo dispositivo de captura: " << snd_strerror(err) << std::endl;
            return false;
        }

        if (!configureDevice(captureHandle))
        {
            std::cerr << "Error configurando dispositivo de captura" << std::endl;
            return false;
        }

        // Configurar reproducción de audio
        err = snd_pcm_open(&playbackHandle, "default", SND_PCM_STREAM_PLAYBACK, 0);
        if (err < 0)
        {
            std::cerr << "Error abriendo dispositivo de reproducción: " << snd_strerror(err) << std::endl;
            return false;
        }

        if (!configureDevice(playbackHandle))
        {
            std::cerr << "Error configurando dispositivo de reproducción" << std::endl;
            return false;
        }

        // Inicializar efectos
        delay = std::make_unique<DelayEffect>(SAMPLE_RATE, 300.0f, 0.4f, 0.3f);
        flanger = std::make_unique<FlangerEffect>(SAMPLE_RATE, 0.5f, 0.7f, 0.3f, 0.4f);
        chorus = std::make_unique<ChorusEffect>(SAMPLE_RATE, 3, 0.8f, 0.3f);

        std::cout << "Procesador de audio inicializado correctamente" << std::endl;
        return true;
    }

private:
    bool configureDevice(snd_pcm_t *handle)
    {
        snd_pcm_hw_params_t *hwParams;
        snd_pcm_hw_params_alloca(&hwParams);

        int err = snd_pcm_hw_params_any(handle, hwParams);
        if (err < 0)
            return false;

        err = snd_pcm_hw_params_set_access(handle, hwParams, SND_PCM_ACCESS_RW_INTERLEAVED);
        if (err < 0)
            return false;

        err = snd_pcm_hw_params_set_format(handle, hwParams, FORMAT);
        if (err < 0)
            return false;

        unsigned int rate = SAMPLE_RATE;
        err = snd_pcm_hw_params_set_rate_near(handle, hwParams, &rate, 0);
        if (err < 0)
            return false;

        err = snd_pcm_hw_params_set_channels(handle, hwParams, 1); // Mono
        if (err < 0)
            return false;

        snd_pcm_uframes_t bufferSize = BUFFER_SIZE;
        err = snd_pcm_hw_params_set_buffer_size_near(handle, hwParams, &bufferSize);
        if (err < 0)
            return false;

        err = snd_pcm_hw_params(handle, hwParams);
        if (err < 0)
            return false;

        return true;
    }

public:
    void start()
    {
        if (running)
            return;

        running = true;
        std::thread audioThread(&AudioProcessor::processAudio, this);
        audioThread.detach();

        std::cout << "Procesamiento de audio iniciado. Presiona 'q' para salir." << std::endl;
    }

    void stop()
    {
        running = false;
    }

private:
    void processAudio()
    {
        std::vector<int16_t> inputBuffer(BUFFER_SIZE);
        std::vector<int16_t> outputBuffer(BUFFER_SIZE);

        while (running)
        {
            // Leer audio de entrada
            int framesRead = snd_pcm_readi(captureHandle, inputBuffer.data(), BUFFER_SIZE);
            if (framesRead < 0)
            {
                snd_pcm_prepare(captureHandle);
                continue;
            }

            // Procesar cada muestra
            for (int i = 0; i < framesRead; ++i)
            {
                // Convertir de int16 a float (-1.0 a 1.0)
                float sample = static_cast<float>(inputBuffer[i]) / 32768.0f;

                // Aplicar efectos en cadena
                sample = delay->process(sample);
                sample = flanger->process(sample);
                sample = chorus->process(sample);

                // Convertir de vuelta a int16
                sample = std::clamp(sample, -1.0f, 1.0f);
                outputBuffer[i] = static_cast<int16_t>(sample * 32767.0f);
            }

            // Escribir audio procesado
            int framesWritten = snd_pcm_writei(playbackHandle, outputBuffer.data(), framesRead);
            if (framesWritten < 0)
            {
                snd_pcm_prepare(playbackHandle);
            }
        }
    }

public:
    // Métodos para controlar efectos en tiempo real
    void setDelayFeedback(float feedback)
    {
        if (delay)
            delay->setFeedback(feedback);
    }

    void setDelayWet(float wet)
    {
        if (delay)
            delay->setWetLevel(wet);
    }

    void setFlangerRate(float rate)
    {
        if (flanger)
            flanger->setRate(rate);
    }

    void setFlangerDepth(float depth)
    {
        if (flanger)
            flanger->setDepth(depth);
    }

    void setChorusDepth(float depth)
    {
        if (chorus)
            chorus->setDepth(depth);
    }

    void setChorusWet(float wet)
    {
        if (chorus)
            chorus->setWetLevel(wet);
    }
};

int main()
{
    AudioProcessor processor;

    if (!processor.initialize())
    {
        std::cerr << "Error inicializando el procesador de audio" << std::endl;
        return 1;
    }

    processor.start();

    // Bucle principal para controles interactivos
    char input;
    while (std::cin >> input)
    {
        switch (input)
        {
        case 'q':
            processor.stop();
            std::cout << "Saliendo..." << std::endl;
            return 0;

        case '1':
            processor.setDelayFeedback(0.2f);
            std::cout << "Delay feedback bajo" << std::endl;
            break;

        case '2':
            processor.setDelayFeedback(0.6f);
            std::cout << "Delay feedback alto" << std::endl;
            break;

        case '3':
            processor.setFlangerRate(0.3f);
            std::cout << "Flanger lento" << std::endl;
            break;

        case '4':
            processor.setFlangerRate(2.0f);
            std::cout << "Flanger rápido" << std::endl;
            break;

        case '5':
            processor.setChorusDepth(0.3f);
            std::cout << "Chorus sutil" << std::endl;
            break;

        case '6':
            processor.setChorusDepth(1.0f);
            std::cout << "Chorus intenso" << std::endl;
            break;

        default:
            std::cout << "Controles: 1-2 (delay), 3-4 (flanger), 5-6 (chorus), q (salir)" << std::endl;
            break;
        }
    }

    return 0;
}