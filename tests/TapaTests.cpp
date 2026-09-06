#include "Plugin.h"
#include "DrumVoice.h"
#include "Presets.h"
#include <clap/ext/draft/webview.h>
#include <clap/ext/gui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace
{
#define check(passed) do { if (!(passed)) { std::fprintf(stderr, "tapa check failed at line %d: %s\n", __LINE__, #passed); std::abort(); } } while (false)
std::string visualMessage;
const clap_host_webview_t webHost { [](const clap_host_t*, const void* data, uint32_t size) -> bool {
    visualMessage.assign(static_cast<const char*>(data), size);
    return true;
}};
const void* CLAP_ABI extension(const clap_host_t*, const char* id)
{
    return std::strcmp(id, CLAP_EXT_WEBVIEW) == 0 ? &webHost : nullptr;
}
void CLAP_ABI noop(const clap_host_t*) {}
const clap_host_t host { CLAP_VERSION, nullptr, "tapa tests", "Char", "", "1", extension, noop, noop, noop };
struct Events
{
    std::vector<const clap_event_header_t*> items;
    clap_input_events_t input { this,
        [](const clap_input_events_t* e) -> uint32_t { return static_cast<Events*>(e->ctx)->items.size(); },
        [](const clap_input_events_t* e, uint32_t i) { return static_cast<Events*>(e->ctx)->items.at(i); } };
};
std::array<double, tapa::parameters.size()> defaults()
{
    std::array<double, tapa::parameters.size()> values {};
    for (size_t i = 0; i < values.size(); ++i) values[i] = tapa::parameters[i].defaultValue;
    return values;
}
void writeWav(const std::vector<float>& samples, const char* filename = "tapa-demo.wav")
{
    std::ofstream file(filename, std::ios::binary);
    auto u16 = [&](uint16_t n) { for (int i = 0; i < 2; ++i) file.put(static_cast<char>(n >> (i * 8))); };
    auto u32 = [&](uint32_t n) { for (int i = 0; i < 4; ++i) file.put(static_cast<char>(n >> (i * 8))); };
    file.write("RIFF", 4); u32(36 + samples.size() * 2); file.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(1); u32(48000); u32(96000); u16(2); u16(16);
    file.write("data", 4); u32(samples.size() * 2);
    for (auto sample : samples) u16(static_cast<uint16_t>(static_cast<int16_t>(std::clamp(sample, -1.0f, 1.0f) * 32767)));
    check(file.good());
}
}

int main()
{
    // Stress the full range and retriggers at multiple rates, including parameter NaNs.
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        tapa::DrumVoice voice;
        voice.prepare(rate);
        for (int mode = 0; mode < 5; ++mode)
        {
            auto values = defaults();
            for (size_t i = 0; i < values.size(); ++i)
                if (mode != 0) values[i] = mode == 1 ? tapa::parameters[i].minimum
                                             : mode == 2 || mode == 4 ? tapa::parameters[i].maximum : NAN;
            if (mode == 4) values[7] = 60;
            voice.trigger(values, 1.0f);
            double energy = 0.0;
            for (int i = 0; i < rate * 16.5; ++i)
            {
                if (i == 137) voice.trigger(values, 0.8f);
                const auto sample = voice.next();
                check(std::isfinite(sample) && std::abs(sample) <= 1.0);
                const auto visual = voice.visualState();
                check(visual[0] >= 0 && visual[0] <= 1 && std::isfinite(visual[1]) && std::isfinite(visual[2]));
                energy += sample * sample;
                if (i > rate * 16.4) check(std::abs(sample) < 1.0e-5);
            }
            check(energy > 0.01);
            voice.reset();
            check(voice.next() == 0.0);
        }
    }

    // Key follow changes body/noise and linked FM times without changing host values.
    for (double rate : {44100.0, 48000.0, 96000.0})
    {
        auto settings = defaults(); settings[0] = 600; settings[6] = 1;
        for (int note : {0, 36, 48, 60, 72, 84, 127})
        {
            tapa::DrumVoice voice; voice.prepare(rate); voice.trigger(settings, 1.0f, note);
            const auto expected = 600 * std::clamp(std::exp2((60.0-note)/24), .5, 2.0);
            check(std::abs(voice.bodyDecayMs() - expected) < 1e-9);
            for (int i = 0; i < rate * expected * .001; ++i) (void) voice.next();
            check(std::abs(voice.visualState()[0] - .001) < .00002);
        }
        tapa::DrumVoice silent; silent.prepare(rate);
        settings[2] = settings[4] = settings[7] = 100; settings[3] = 12;
        silent.trigger(settings, 0.0f, 36);
        for (int i = 0; i < 4096; ++i) check(silent.next() == 0);
    }

    // Saturation changes harmonics without substantially stretching the audible decay.
    {
        std::array<double, 2> falls {};
        for (int mode = 0; mode < 2; ++mode)
        {
            tapa::DrumVoice voice; voice.prepare(48000);
            auto settings = defaults();
            settings[0] = 600; settings[1] = settings[2] = 0; settings[3] = mode * 12;
            voice.trigger(settings, 1.0f, 60);
            double early = 0, late = 0;
            for (int i = 0; i < 15360; ++i)
            {
                const auto sample = voice.next();
                if (i >= 4800 && i < 5760) early += sample * sample;
                if (i >= 14400) late += sample * sample;
            }
            falls[mode] = 10 * std::log10(late / early);
        }
        check(std::abs(falls[0] - falls[1]) < .5);
    }

    // Excited metal rings after its input stops, resets exactly, and remains bounded.
    for (double rate : {44100.0, 48000.0, 96000.0})
    {
        tapa::ResonatorBank metal;
        metal.trigger(rate * 4, 420, 2.6, .7, 600, 1, 1);
        double ringingEnergy = 0;
        for (int i = 0; i < rate * 4 * .1; ++i)
        {
            const auto sample = metal.next(i == 0 ? 1 : 0);
            check(std::isfinite(sample));
            if (i > rate * 4 * .01) ringingEnergy += sample * sample;
        }
        check(ringingEnergy > .001);
        metal.reset();
        check(metal.next(0) == 0);
        for (int note : {0, 60, 127})
        {
            tapa::DrumVoice voice, fresh; voice.prepare(rate); fresh.prepare(rate);
            auto settings = defaults();
            settings[0] = 150; settings[1] = settings[2] = settings[4] = 100;
            settings[3] = 12; settings[5] = 16; settings[7] = 60;
            voice.trigger(settings, 1, note);
            double energy = 0;
            for (int i = 0; i < rate; ++i)
            {
                const auto sample = voice.next();
                check(std::isfinite(sample) && std::abs(sample) <= .5);
                energy += sample * sample;
                if (i > rate * .95) check(std::abs(sample) < 1e-5);
            }
            check(energy > .001);
            voice.reset(); voice.trigger(settings,1,note); fresh.trigger(settings,1,note);
            for (int i = 0; i < 2048; ++i) check(voice.next() == fresh.next());
        }
    }

    // Feedback must change the sound and reset deterministically on retrigger.
    {
        tapa::DrumVoice clean, driven, fresh;
        clean.prepare(48000); driven.prepare(48000); fresh.prepare(48000);
        auto settings = defaults();
        clean.trigger(settings, 1.0f);
        settings[4] = 85;
        driven.trigger(settings, 1.0f);
        double difference = 0;
        for (int i = 0; i < 12000; ++i) difference += std::abs(clean.next() - driven.next());
        check(difference > 1.0);
        driven.reset();
        driven.trigger(settings, 1.0f); fresh.trigger(settings, 1.0f);
        for (int i = 0; i < 4096; ++i) check(driven.next() == fresh.next());
    }

    // Ratio changes timbre only when a modulator is audible.
    for (double amount : {0.0, 65.0})
    {
        tapa::DrumVoice low, high;
        low.prepare(48000); high.prepare(48000);
        auto settings = defaults(); settings[1] = amount; settings[2] = 0;
        settings[5] = 1; low.trigger(settings, 1.0f);
        settings[5] = 7.3; high.trigger(settings, 1.0f);
        double difference = 0;
        for (int i = 0; i < 4096; ++i) difference += std::abs(low.next() - high.next());
        check(amount == 0 ? difference == 0 : difference > 1.0);
    }
    // The transient macro changes waveform character, rather than just gain.
    {
        tapa::DrumVoice tick, metal, dry;
        tick.prepare(48000); metal.prepare(48000); dry.prepare(48000);
        auto settings = defaults(); settings[1] = 0;
        settings[2] = 0; dry.trigger(settings, 1.0f);
        settings[2] = 35; tick.trigger(settings, 1.0f);
        settings[2] = 100; metal.trigger(settings, 1.0f);
        double aa = 0, bb = 0, ab = 0;
        for (int i = 0; i < 1500; ++i)
        {
            const auto baseline = dry.next();
            const auto a = tick.next() - baseline, b = metal.next() - baseline;
            aa += a * a; bb += b * b; ab += a * b;
        }
        check(ab / std::sqrt(aa * bb) < .98);
    }

    // Linked mode retains FM in a long tail; Body mode lets it settle to sine.
    {
        double modulation[2] {};
        for (int linked = 0; linked < 2; ++linked)
        {
            tapa::DrumVoice voice; voice.prepare(48000);
            auto settings = defaults(); settings[0] = 2000; settings[6] = linked;
            voice.trigger(settings, 1.0f);
            for (int i = 0; i < 24000; ++i)
            {
                (void) voice.next();
                if (i >= 19200) modulation[linked] += std::abs(voice.visualState()[1]);
            }
        }
        check(modulation[0] < 1e-8 && modulation[1] > 100);
    }

    // Noise can sustain after the attack, and follows the selected decay.
    {
        tapa::DrumVoice tonal, noisy;
        tonal.prepare(48000); noisy.prepare(48000);
        auto settings = defaults(); settings[0] = 2000; settings[1] = settings[2] = 0;
        tonal.trigger(settings, 1.0f);
        settings[7] = 100; noisy.trigger(settings, 1.0f);
        int tonalCrossings = 0, noiseCrossings = 0;
        double previousTone = 0, previousNoise = 0, tailEnergy = 0;
        for (int i = 0; i < 24000; ++i)
        {
            const auto tone = tonal.next(), noise = noisy.next();
            if (i > 12000)
            {
                tonalCrossings += previousTone <= 0 && tone > 0;
                noiseCrossings += previousNoise <= 0 && noise > 0;
                tailEnergy += noise * noise;
            }
            previousTone = tone; previousNoise = noise;
        }
        check(tailEnergy > .01 && noiseCrossings > tonalCrossings * 3);
        noisy.reset();
        tapa::DrumVoice fresh; fresh.prepare(48000);
        noisy.trigger(settings, 1.0f); fresh.trigger(settings, 1.0f);
        for (int i = 0; i < 1024; ++i) check(noisy.next() == fresh.next());
    }

    // FM must not introduce a pitched oscillator into the sustained noise.
    {
        tapa::DrumVoice plain, modulated;
        plain.prepare(48000); modulated.prepare(48000);
        auto settings = defaults();
        settings[0] = 2000; settings[1] = settings[2] = 0; settings[7] = 100;
        plain.trigger(settings, 1.0f);
        settings[1] = settings[4] = 100; settings[6] = 1;
        modulated.trigger(settings, 1.0f);
        for (int i = 0; i < 48000; ++i) check(plain.next() == modulated.next());
    }

    // One octave on MIDI must double the settled body frequency.
    auto values = defaults();
    values[0] = 900; values[1] = values[2] = 0;
    std::array<int, 2> crossings {};
    for (int octave = 0; octave < 2; ++octave)
    {
        tapa::DrumVoice voice;
        voice.prepare(48000);
        voice.trigger(values, 1.0f, 36 + 12 * octave);
        double previous = 0;
        for (int frame = 0; frame < 24000; ++frame)
        {
            const auto sample = voice.next();
            if (frame > 4800 && previous <= 0 && sample > 0) ++crossings[octave];
            previous = sample;
        }
    }
    check(std::abs(crossings[1] - 2 * crossings[0]) <= 1);
    check(std::abs(crossings[0] - 26) <= 1); // MIDI 36 = 65.406 Hz over 0.4 seconds.

    const auto* factory = static_cast<const clap_plugin_factory_t*>(tapa::entryGetFactory(CLAP_PLUGIN_FACTORY_ID));
    check(factory && factory->get_plugin_count(factory) == 1);
    const auto* plugin = factory->create_plugin(factory, &host, tapa::descriptor().id);
    check(plugin && plugin->init(plugin));
    const auto* gui = static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
    check(gui && gui->create(plugin, CLAP_WINDOW_API_WEBVIEW, false));
    // AUv3 layout forwards host bounds directly, including small transient sizes.
    for (const auto size : { std::array<uint32_t, 2>{ 1, 1 }, { 100, 100 },
                             { 419, 460 }, { 640, 219 }, { 420, 220 }, { 640, 460 } })
    {
        check(gui->set_size(plugin, size[0], size[1]));
        uint32_t w = 0, h = 0;
        check(gui->get_size(plugin, &w, &h) && w == size[0] && h == size[1]);
        check(gui->adjust_size(plugin, &w, &h) && w == size[0] && h == size[1]);
    }
    clap_gui_resize_hints_t hints {};
    check(gui->get_resize_hints(plugin, &hints) && hints.can_resize_horizontally
          && hints.can_resize_vertically && !hints.preserve_aspect_ratio);
    uint32_t width = 1, height = 1;
    check(gui->adjust_size(plugin, &width, &height) && width == 1 && height == 1);
    check(gui->set_size(plugin, 1280, 800));
    check(gui->get_size(plugin, &width, &height) && width == 1280 && height == 800);
    check(!gui->set_size(plugin, 0, 800) && !gui->set_size(plugin, 1280, 0));
    check(gui->get_size(plugin, &width, &height) && width == 1280 && height == 800);
    width = 0;
    check(!gui->adjust_size(plugin, &width, &height));
    check(!gui->adjust_size(plugin, nullptr, &height));
    check(!gui->adjust_size(plugin, &width, nullptr));
    gui->destroy(plugin);

    const auto* params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    const auto* state = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    const auto* presets = static_cast<const clap_plugin_preset_load_t*>(plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD));
    check(params && state && presets && params->count(plugin) == tapa::parameters.size());
    for (uint32_t i = 0; i < tapa::parameters.size(); ++i)
    {
        clap_param_info_t info {};
        double value = 0;
        check(params->get_info(plugin, i, &info) && info.id == tapa::parameters[i].id);
        check(params->get_value(plugin, tapa::parameters[i].id, &value) && value == tapa::parameters[i].defaultValue);
    }
    std::vector<char> saved;
    clap_ostream_t out { &saved, [](const clap_ostream_t* s, const void* data, uint64_t n) -> int64_t {
        auto& bytes = *static_cast<std::vector<char>*>(s->ctx);
        const auto count = std::min<uint64_t>(n, 7);
        bytes.insert(bytes.end(), static_cast<const char*>(data), static_cast<const char*>(data) + count);
        return count;
    }};
    check(state->save(plugin, &out));
    check(presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "deep"));
    struct Reader { const std::vector<char>& bytes; size_t pos = 0; } reader { saved };
    clap_istream_t in { &reader, [](const clap_istream_t* s, void* data, uint64_t n) -> int64_t {
        auto& r = *static_cast<Reader*>(s->ctx);
        const auto count = std::min<uint64_t>({n, 5, r.bytes.size() - r.pos});
        std::memcpy(data, r.bytes.data() + r.pos, count); r.pos += count; return count;
    }};
    check(state->load(plugin, &in));
    // Old four-control states load with feedback off; new states retain it.
    for (uint32_t version : {1u, 2u, 3u, 4u, 5u})
    {
        std::vector<char> legacy;
        const auto append = [&](const auto& value) {
            const auto* bytes = reinterpret_cast<const char*>(&value);
            legacy.insert(legacy.end(), bytes, bytes + sizeof(value));
        };
        append(uint32_t {0x42444b31}); append(version);
        if (version == 1) append(double {0});
        for (double value : {280.0, 50.0, 30.0, 2.0}) append(value);
        if (version >= 3) append(double {0});
        if (version >= 4) append(double {2.6025});
        if (version == 5) append(double {0});
        check(presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "fracture"));
        Reader oldReader {legacy}; in.ctx = &oldReader;
        check(state->load(plugin, &in));
        double feedback = -1;
        check(params->get_value(plugin, 5, &feedback) && feedback == 0);
        double noise = -1;
        check(params->get_value(plugin, 8, &noise) && noise == 0);
        double linked = -1;
        check(params->get_value(plugin, 7, &linked) && linked == 0);
        double ratio = 0;
        check(params->get_value(plugin, 6, &ratio) && std::abs(ratio - 2.6025) < 1e-9);
    }
    saved.clear();
    check(presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "fracture"));
    check(state->save(plugin, &out));
    check(presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "round"));
    reader.pos = 0; in.ctx = &reader;
    check(state->load(plugin, &in));
    double feedback = 0;
    check(params->get_value(plugin, 5, &feedback) && feedback == 72);
    check(presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "round"));
    saved.clear();
    check(presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "long"));
    check(state->save(plugin, &out));
    check(presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "round"));
    reader.pos = 0;
    check(state->load(plugin, &in));
    double linked = 0;
    check(params->get_value(plugin, 7, &linked) && linked == 1);
    check(presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "round"));
    char feedbackText[32] {};
    check(params->value_to_text(plugin, 5, 72, feedbackText, sizeof(feedbackText)));
    check(std::strstr(feedbackText, "%") != nullptr);
    double decay = 0;
    check(params->get_value(plugin, 1, &decay) && decay == 280);
    check(plugin->activate(plugin, 48000, 1, 512) && plugin->start_processing(plugin));
    clap_event_note_t note { { sizeof(note), 73, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0 }, -1, 0, 0, 36, 1.0 };
    Events events { { &note.header } };
    std::array<float,512> left {}, right {};
    float* channels[] { left.data(), right.data() };
    clap_audio_buffer_t buffer { channels, nullptr, 2, 0, 0 };
    clap_process_t process {};
    process.frames_count = 512; process.audio_outputs = &buffer; process.audio_outputs_count = 1; process.in_events = &events.input;
    check(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR);
    for (size_t i = 0; i < 73; ++i) check(left[i] == 0.0f);
    check(std::any_of(left.begin() + 74, left.end(), [](auto v) { return std::abs(v) > 0.01; }));
    check(left == right);
    // Reset restores exact phase and output; the 64-bit render matches the float path.
    plugin->reset(plugin);
    std::array<double,512> dl {}, dr {};
    double* doubles[] { dl.data(), dr.data() };
    buffer.data32 = nullptr; buffer.data64 = doubles;
    check(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR);
    for (size_t i = 0; i < left.size(); ++i) check(std::abs(left[i] - dl[i]) < 1.0e-6);
    plugin->stop_processing(plugin); plugin->deactivate(plugin);

    // Audition file comes through the actual CLAP process path: Round, Dry, Deep.
    check(plugin->activate(plugin, 48000, 1, 512) && plugin->start_processing(plugin));
    buffer.data32 = channels; buffer.data64 = nullptr;
    const auto* web = static_cast<const clap_plugin_webview_t*>(plugin->get_extension(plugin, CLAP_EXT_WEBVIEW));
    check(web != nullptr);
    char uri[128] {};
    check(web->get_uri(plugin, uri, sizeof(uri)) > 0);
    std::vector<float> demo;
    float maxVisualEnergy = 0, lastVisualEnergy = 0;
    unsigned maxVisualHit = 0;
    std::string activeVisual;

    for (const auto* key : { "round", "dry", "deep" })
    {
        check(presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key));
        double baseDecay = 0;
        check(params->get_value(plugin, 1, &baseDecay));
        for (int hit = 0; hit < 4; ++hit)
        {
            note.header.time = 0; note.velocity = hit == 2 ? 0.65 : 1.0;
            events.items = { &note.header };
            for (int block = 0; block < 75; ++block)
            {
                check(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR);
                demo.insert(demo.end(), left.begin(), left.end());
                visualMessage.clear();
                check(web->receive(plugin, "visual", 6));
                unsigned hit = 0;
                float energy = 0, peak = 0, edge = 0, decayMs = 0, punch = 0, click = 0, drive = 0;
                if (std::sscanf(visualMessage.c_str(), "visual:%u,%f,%f,%f,%f,%f,%f,%f", &hit, &energy, &peak, &edge, &decayMs, &punch, &click, &drive) == 8)
                {
                    check(std::isfinite(energy) && energy >= 0 && peak <= 0.5 && edge >= 0);
                    check(std::count(visualMessage.begin(), visualMessage.end(), ',') == 79);
                    const auto expectedDecay = baseDecay * std::clamp(std::exp2((60.0-note.key)/24), .5, 2.0);
                    check(std::abs(decayMs - expectedDecay) < .001);
                    check(punch >= 0 && punch <= 1 && click >= 0 && click <= 1 && drive >= 0 && drive <= 1);
                    if (energy > maxVisualEnergy) { maxVisualEnergy = energy; activeVisual = visualMessage; }
                    lastVisualEnergy = energy;
                    maxVisualHit = std::max(maxVisualHit, hit);
                }
                events.items.clear();
            }
        }
    }
    check(maxVisualEnergy > 0.05f && lastVisualEnergy < 0.005f && maxVisualHit >= 12);
    std::ofstream("tapa-visual.txt") << activeVisual;
    writeWav(demo);
    std::vector<float> presetDemo;
    for (const auto& preset : tapa::presets)
    {
        check(std::count_if(tapa::presets.begin(), tapa::presets.end(), [&](const auto& other) {
            return std::strcmp(other.key,preset.key)==0 || std::strcmp(other.name,preset.name)==0;
        }) == 1);
        for (size_t i = 0; i < preset.values.size(); ++i)
            check(std::isfinite(preset.values[i]) && preset.values[i] >= tapa::parameters[i].minimum
                                                && preset.values[i] <= tapa::parameters[i].maximum);
        if (preset.suggestedNote < 0) continue;
        check(presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, preset.key));
        for (size_t i = 0; i < preset.values.size(); ++i)
        {
            double value = 0;
            check(params->get_value(plugin, tapa::parameters[i].id, &value));
            check(std::abs(value - preset.values[i]) < 1e-8);
        }
        plugin->reset(plugin);
        tapa::DrumVoice reference; reference.prepare(48000);
        reference.trigger(preset.values, 1.0f, preset.suggestedNote);
        note.header.time = 0; note.velocity = 1; note.key = preset.suggestedNote;
        events.items = { &note.header };
        double energy = 0;
        for (int block = 0; block < 320; ++block)
        {
            check(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR);
            for (auto sample : left)
            {
                const auto expected = reference.next();
                check(std::isfinite(sample) && std::abs(sample) <= .5);
                check(std::abs(sample - expected) < 1e-6);
                energy += sample * sample;
            }
            presetDemo.insert(presetDemo.end(), left.begin(), left.end());
            events.items.clear();
        }
        check(energy > .001);
    }
    writeWav(presetDemo, "tapa-presets-demo.wav");
    // The open hat must retain a clearly longer tail; crash blooms later than ride.
    std::array<double, 4> halfTimes {}, tailTimes {};
    const std::array keys {"closed-hat", "open-hat", "crash", "ride"};
    for (size_t i = 0; i < keys.size(); ++i)
    {
        const auto preset = std::find_if(tapa::presets.begin(), tapa::presets.end(),
            [&](const auto& p) { return std::strcmp(p.key, keys[i]) == 0; });
        check(preset != tapa::presets.end());
        tapa::DrumVoice voice; voice.prepare(48000); voice.trigger(preset->values,1,preset->suggestedNote);
        std::vector<double> cumulative(96000);
        double energy = 0;
        for (auto& value : cumulative)
        {
            const auto sample = voice.next(); energy += sample * sample; value = energy;
        }
        halfTimes[i] = std::distance(cumulative.begin(), std::lower_bound(cumulative.begin(), cumulative.end(), energy * .5)) / 48000.0;
        tailTimes[i] = std::distance(cumulative.begin(), std::lower_bound(cumulative.begin(), cumulative.end(), energy * .99)) / 48000.0;
    }
    check(tailTimes[1] > tailTimes[0] * 2);
    check(halfTimes[2] > halfTimes[3]);
    // Snare needs an audible noisy body after the initial tick.
    {
        const auto preset = std::find_if(tapa::presets.begin(),tapa::presets.end(),
            [](const auto& p) { return std::strcmp(p.key,"snare")==0; });
        check(preset != tapa::presets.end());
        tapa::DrumVoice voice; voice.prepare(48000); voice.trigger(preset->values,1,preset->suggestedNote);
        double total = 0, buzz = 0;
        int crossings = 0;
        double previous = 0;
        for (int i = 0; i < 48000; ++i)
        {
            const auto sample = voice.next(); total += sample * sample;
            if (i >= 2400 && i < 9600)
            {
                buzz += sample * sample;
                crossings += previous <= 0 && sample > 0;
            }
            previous = sample;
        }
        check(buzz > total * .1 && crossings > 100);
    }
    // A clap has distinct early strikes followed by noise, rather than one hat-like wash.
    {
        const auto preset = std::find_if(tapa::presets.begin(), tapa::presets.end(),
            [](const auto& p) { return std::strcmp(p.key, "clap") == 0; });
        check(preset != tapa::presets.end());
        for (double rate : {44100.0, 48000.0, 96000.0})
            for (int note : {36, 60, 84})
            {
                tapa::DrumVoice voice; voice.prepare(rate); voice.trigger(preset->values, 1, note);
                std::array<double, 5> energy {};
                constexpr std::array starts {0.0, .008, .013, .021, .026};
                double tail = 0;
                for (int i = 0; i < rate; ++i)
                {
                    const auto sample = voice.next();
                    check(std::isfinite(sample) && std::abs(sample) <= .5);
                    const auto time = i / rate;
                    for (size_t j = 0; j < starts.size(); ++j)
                        if (time >= starts[j] && time < starts[j] + .003)
                            energy[j] += sample * sample;
                    if (time >= .04 && time < .15) tail += sample * sample;
                }
                check(energy[0] > energy[1] * 4 && energy[2] > energy[1] * 4);
                check(energy[2] > energy[3] * 4 && energy[4] > energy[3] * 4);
                check(tail > energy[4]);
            }
    }
    plugin->stop_processing(plugin); plugin->deactivate(plugin); plugin->destroy(plugin);
    std::puts("tapa: DSP bounds/tails, MIDI pitch, CLAP timing, stereo/64-bit, state and presets passed");
}
