#include "Presets.h"
#include "Plugin.h"

#include "char_clap_utils/Streams.h"
#include "char_clap_utils/WebUI.h"

#include "char_clap_utils/EventChunks.h"
#include "char_clap_utils/ParameterState.h"
#include "char_clap_utils/Process.h"

#include "Kick.h"
#include "Parameters.h"

#include <clap/helpers/param-queue.hh>
#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>
#include <clap/ext/event-registry.h>
#include <clap/ext/param-indication.h>
#include <clap/ext/preset-load.h>
#include <clap/factory/preset-discovery.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace bd
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.bd";

enum class EditType : uint8_t { begin, value, end };
struct Edit { EditType type; clap_id id; double value; };
struct UINote { bool on; int note; float velocity; };

class BDPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit BDPlugin(const clap_host_t* host)
        : Base(&descriptor(), host),
          host(host),
          ui(host, [this](std::string_view message) { return receiveUI(message); })
    {
    }

protected:
    bool init() noexcept override
    {
        hostParams = static_cast<const clap_host_params_t*>(host->get_extension(host, CLAP_EXT_PARAMS));
        hostState = static_cast<const clap_host_state_t*>(host->get_extension(host, CLAP_EXT_STATE));
        hostPresetLoad = static_cast<const clap_host_preset_load_t*>(
            host->get_extension(host, CLAP_EXT_PRESET_LOAD));

        return true;
    }

    bool activate(double sampleRate, uint32_t, uint32_t) noexcept override
    {
        kick.prepare(sampleRate);
        visualInterval = std::max(1u, static_cast<uint32_t>(sampleRate / 30.0));
        visualFrame = {};
        visualFrames = 0;
        visualPrevious = 0;
        return true;
    }

    void reset() noexcept override { kick.reset(); }
    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (process == nullptr || process->audio_outputs_count == 0)
            return CLAP_PROCESS_ERROR;

        for (auto& state : states) (void) state.consumePublishedBase();

        UINote note;
        while (uiNotes.tryPop(note))
            applyNote(note.on, note.note, note.velocity);

        const char_clap::ProcessView view { *process };
        char_clap::processEventChunks(
            view.inputEvents(), view.frameCount(),
            [this](const clap_event_header_t& event) noexcept { applyEvent(event); },
            [this, &view](uint32_t begin, uint32_t end) noexcept { render(view, begin, end); });

        emitEdits(view.outputEvents());
        return CLAP_PROCESS_CONTINUE;
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override { return isInput ? 0u : 1u; }

    bool audioPortsInfo(uint32_t index, bool isInput, clap_audio_port_info_t* info) const noexcept override
    {
        if (isInput || index != 0 || info == nullptr) return false;
        *info = {};
        info->id = 0x53594f55;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN | CLAP_AUDIO_PORT_SUPPORTS_64BITS;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf(info->name, sizeof(info->name), "Stereo Output");
        return true;
    }

    bool implementsNotePorts() const noexcept override { return true; }
    uint32_t notePortsCount(bool isInput) const noexcept override { return isInput ? 1u : 0u; }

    bool notePortsInfo(uint32_t index, bool isInput, clap_note_port_info_t* info) const noexcept override
    {
        if (!isInput || index != 0 || info == nullptr) return false;
        *info = {};
        info->id = 0x42444b31;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        std::snprintf(info->name, sizeof(info->name), "Notes");
        return true;
    }

    bool implementsParams() const noexcept override { return true; }
    uint32_t paramsCount() const noexcept override { return static_cast<uint32_t>(parameters.size()); }

    bool paramsInfo(uint32_t index, clap_param_info_t* info) const noexcept override
    {
        if (index >= parameters.size() || info == nullptr) return false;
        const auto& parameter = parameters[index];
        *info = {};
        info->id = parameter.id;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE;
        if (parameter.id == 7) info->flags |= CLAP_PARAM_IS_STEPPED;
        info->min_value = parameter.minimum;
        info->max_value = parameter.maximum;
        info->default_value = parameter.defaultValue;
        std::snprintf(info->name, sizeof(info->name), "%s", parameter.name);
        return true;
    }

    bool paramsValue(clap_id id, double* value) noexcept override
    {
        if (value == nullptr) return false;
        const auto* state = stateFor(id);
        if (state == nullptr) return false;
        *value = state->baseValueForMainThread();
        return true;
    }

    bool paramsValueToText(clap_id id, double value, char* text, uint32_t size) noexcept override
    {
        if (stateFor(id) == nullptr || text == nullptr || size == 0) return false;
        if (id == 7) return std::snprintf(text, size, "%s", value >= .5 ? "Linked" : "Body") > 0;
        return std::snprintf(text, size, "%.2f %s", value, parameters[id - 1].unit) > 0;
    }

    bool paramsTextToValue(clap_id id, const char* text, double* value) noexcept override
    {
        auto* state = stateFor(id);
        if (!state || !text || !value) return false;
        if (id == 7 && (std::strcmp(text, "Body") == 0 || std::strcmp(text, "Linked") == 0))
        {
            *value = std::strcmp(text, "Linked") == 0 ? 1.0 : 0.0;
            return true;
        }
        char* end = nullptr;
        const auto parsed = std::strtod(text, &end);
        if (end == text || !std::isfinite(parsed)) return false;
        *value = state->clamp(parsed);
        return true;
    }

    bool implementsParamIndication() const noexcept override { return true; }

    void paramIndicationSetMapping(clap_id paramId,
                                   bool hasMapping,
                                   const clap_color_t*,
                                   const char* label,
                                   const char*) noexcept override
    {
        if (!stateFor(paramId)) return;

        char message[256] {};
        std::snprintf(message, sizeof(message), "param-indication:%u:%u:%s",
                      static_cast<unsigned>(paramId), hasMapping ? 1u : 0u,
                      label != nullptr ? label : "");
        ui.send(message);
    }

    void paramsFlush(const clap_input_events_t* input, const clap_output_events_t* output) noexcept override
    {
        const char_clap::InputEventsView events { input };
        for (uint32_t i = 0; i < events.size(); ++i)
            if (const auto* event = events[i]) applyEvent(*event);
        emitEdits(char_clap::OutputEventsView { output });
    }

    bool implementsState() const noexcept override { return true; }

    bool stateSave(const clap_ostream_t* stream) noexcept override
    {
        if (stream == nullptr) return false;
        State state { 0x42444b31, 6, {} };
        for (size_t i = 0; i < parameters.size(); ++i)
            state.values[i] = stateFor(parameters[i].id)->baseValueForMainThread();
        return char_clap::writeComplete(*stream, &state, sizeof(state));
    }

    bool stateLoad(const clap_istream_t* stream) noexcept override
    {
        State state {};
        if (stream == nullptr
            || !char_clap::readComplete(*stream, &state, 2 * sizeof(uint32_t))
            || state.magic != 0x42444b31)
            return false;

        if (state.version == 1)
        {
            std::array<double, 5> legacy {};
            if (!char_clap::readComplete(*stream, legacy.data(), sizeof(legacy))) return false;
            std::copy(legacy.begin() + 1, legacy.end(), state.values.begin());
        }
        else if (state.version == 2)
        {
            std::array<double, 4> legacy {};
            if (!char_clap::readComplete(*stream, legacy.data(), sizeof(legacy))) return false;
            std::copy(legacy.begin(), legacy.end(), state.values.begin());
        }
        else if (state.version == 3)
        {
            std::array<double, 5> legacy {};
            if (!char_clap::readComplete(*stream, legacy.data(), sizeof(legacy))) return false;
            std::copy(legacy.begin(), legacy.end(), state.values.begin());
        }
        else if (state.version == 4)
        {
            std::array<double, 6> legacy {};
            if (!char_clap::readComplete(*stream, legacy.data(), sizeof(legacy))) return false;
            std::copy(legacy.begin(), legacy.end(), state.values.begin());
        }
        else if (state.version == 5)
        {
            std::array<double, 7> legacy {};
            if (!char_clap::readComplete(*stream, legacy.data(), sizeof(legacy))) return false;
            std::copy(legacy.begin(), legacy.end(), state.values.begin());
        }
        else if (state.version != 6
                 || !char_clap::readComplete(*stream, state.values.data(), sizeof(state.values)))
            return false;
        if (state.version < 4)
        {
            const auto amount = std::isfinite(state.values[1]) ? std::clamp(state.values[1], 0.0, 100.0) * .01 : .5;
            state.values[5] = 1.0 + 6.41 * amount * amount;
        }
        for (size_t i = 0; i < parameters.size(); ++i)
            publishParameter(parameters[i].id, state.values[i]);
        notifyValuesChanged();
        return true;
    }

    bool implementsPresetLoad() const noexcept override { return true; }

    bool presetLoadFromLocation(uint32_t locationKind, const char* location,
                                const char* loadKey) noexcept override
    {
        if (locationKind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || location != nullptr
            || loadKey == nullptr)
            return false;
        const auto preset = std::find_if(presets.begin(), presets.end(),
            [loadKey](const auto& item) { return std::strcmp(item.key, loadKey) == 0; });
        if (preset == presets.end()) return false;
        for (size_t i = 0; i < parameters.size(); ++i)
            publishParameter(parameters[i].id, preset->values[i]);
        notifyValuesChanged();
        if (hostPresetLoad) hostPresetLoad->loaded(host, locationKind, location, loadKey);
        return true;
    }

    bool enableDraftExtensions() const noexcept override { return true; }
    bool implementsWebview() const noexcept override { return true; }
    int32_t webviewGetUri(char* uri, uint32_t capacity) const noexcept override
    {
        return ui.getUri(uri, capacity);
    }
    bool webviewGetResource(const char* path, char* mime, uint32_t mimeCapacity,
                            const clap_ostream_t* stream) override
    {
        return ui.getResource(path, mime, mimeCapacity, stream);
    }
    bool webviewReceive(const void* data, uint32_t size) const noexcept override
    {
        return ui.receiveBytes(data, size);
    }

    bool implementsGui() const noexcept override { return true; }
    bool guiIsApiSupported(const char* api, bool floating) noexcept override
    {
        return ui.guiIsApiSupported(api, floating);
    }
    bool guiGetPreferredApi(const char** api, bool* floating) noexcept override
    {
        return ui.guiGetPreferredApi(api, floating);
    }
    bool guiCreate(const char* api, bool floating) noexcept override
    {
        return ui.guiCreate(api, floating, 640, 460);
    }
    void guiDestroy() noexcept override { ui.guiDestroy(); }
    bool guiShow() noexcept override { return ui.guiShow(); }
    bool guiHide() noexcept override { return ui.guiHide(); }
    bool guiGetSize(uint32_t* width, uint32_t* height) noexcept override
    {
        return ui.guiGetSize(width, height);
    }
    bool guiCanResize() const noexcept override { return true; }
    bool guiGetResizeHints(clap_gui_resize_hints_t* hints) noexcept override
    {
        if (!hints) return false;
        *hints = { true, true, false, 0, 0 };
        return true;
    }
    bool guiAdjustSize(uint32_t* width, uint32_t* height) noexcept override
    {
        if (!width || !height) return false;
        *width = std::max(420u, *width);
        *height = std::max(220u, *height);
        return true;
    }
    bool guiSetSize(uint32_t width, uint32_t height) noexcept override
    {
        if (width < 420 || height < 220) return false;
        return ui.guiSetSize(width, height);
    }
    bool guiSetParent(const clap_window_t* window) noexcept override
    {
        return ui.guiSetParent(window);
    }

    void onMainThread() noexcept override
    {
        if (uiDirty.exchange(false, std::memory_order_acq_rel)) sendValues();
    }

private:
    struct State
    {
        uint32_t magic;
        uint32_t version;
        std::array<double, parameters.size()> values;
    };

    char_clap::ParameterState* stateFor(clap_id id) noexcept
    {
        return id > 0 && id <= states.size() ? &states[id - 1] : nullptr;
    }

    void publishParameter(clap_id id, double value) noexcept
    {
        if (auto* state = stateFor(id)) state->publishBase(value);
    }

    void notifyValuesChanged() noexcept
    {
        uiDirty.store(true, std::memory_order_release);
        if (hostParams) hostParams->rescan(host, CLAP_PARAM_RESCAN_VALUES);
        host->request_callback(host);
    }

    bool receiveUI(std::string_view message)
    {
        if (message.substr(0, 7) == "preset:")
            return presetLoadFromLocation(CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr,
                                          std::string(message.substr(7)).c_str());
        if (message == "visual")
        {
            VisualFrame latest {}, frame {};
            bool available = false;
            for (unsigned i = 0; i < 4 && visualQueue.tryPop(frame); ++i)
            {
                latest = frame;
                available = true;
            }
            if (available)
            {
                std::string text = "visual:" + std::to_string(latest.hit)
                                 + "," + std::to_string(latest.energy)
                                 + "," + std::to_string(latest.peak)
                                 + "," + std::to_string(latest.edge)
                                 + "," + std::to_string(latest.decayMs)
                                 + "," + std::to_string(latest.punch)
                                 + "," + std::to_string(latest.click)
                                 + "," + std::to_string(latest.drive);
                for (const auto& snapshot : latest.operators)
                    for (auto value : snapshot) text += "," + std::to_string(value);
                ui.send(text);
            }
            return true;
        }
        if (message == "ready") { sendValues(); return true; }

        int id = -1;
        double value = 0.0;
        int note = -1;
        if (std::sscanf(std::string(message).c_str(), "begin:%d", &id) == 1)
            return queueEdit({ EditType::begin, static_cast<clap_id>(id), 0.0 });
        if (std::sscanf(std::string(message).c_str(), "value:%d:%lf", &id, &value) == 2)
        {
            auto* state = stateFor(static_cast<clap_id>(id));
            if (!state) return false;
            value = state->clamp(value);
            publishParameter(static_cast<clap_id>(id), value);
            if (hostState) hostState->mark_dirty(host);
            notifyValuesChanged();
            return queueEdit({ EditType::value, static_cast<clap_id>(id), value });
        }
        if (std::sscanf(std::string(message).c_str(), "end:%d", &id) == 1)
            return queueEdit({ EditType::end, static_cast<clap_id>(id), 0.0 });
        if (std::sscanf(std::string(message).c_str(), "note-on:%d:%lf", &note, &value) == 2)
        {
            const auto ok = uiNotes.tryPush({ true, note, static_cast<float>(std::clamp(value, 0.0, 1.0)) });
            if (ok) host->request_process(host);
            return ok;
        }
        if (std::sscanf(std::string(message).c_str(), "note-off:%d", &note) == 1)
        {
            const auto ok = uiNotes.tryPush({ false, note, 0.0f });
            if (ok) host->request_process(host);
            return ok;
        }
        return false;
    }

    bool queueEdit(const Edit& edit)
    {
        if (!stateFor(edit.id) || !edits.tryPush(edit)) return false;
        if (hostParams) hostParams->request_flush(host);
        host->request_process(host);
        return true;
    }

    void sendValues() const
    {
        std::string text = "values:";
        for (size_t i = 0; i < states.size(); ++i)
            text += std::to_string(parameters[i].id) + "=" + std::to_string(states[i].baseValueForMainThread()) + ";";
        ui.send(text);
    }

    void emitEdits(char_clap::OutputEventsView output) noexcept
    {
        Edit edit;
        while (edits.tryPeek(edit))
        {
            bool pushed = false;
            if (edit.type == EditType::value)
            {
                const clap_event_param_value_t event {
                    { sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, CLAP_EVENT_IS_LIVE },
                    edit.id, nullptr, -1, -1, -1, -1, edit.value
                };
                pushed = output.tryPush(event);
            }
            else
            {
                const clap_event_param_gesture_t event {
                    { sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID,
                      static_cast<uint16_t>(edit.type == EditType::begin
                                                ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                                : CLAP_EVENT_PARAM_GESTURE_END),
                      CLAP_EVENT_IS_LIVE },
                    edit.id
                };
                pushed = output.tryPush(event);
            }
            if (!pushed) break;
            edits.consume();
        }
    }

    void applyEvent(const clap_event_header_t& event) noexcept
    {
        if (event.space_id != CLAP_CORE_EVENT_SPACE_ID) return;

        if (event.type == CLAP_EVENT_PARAM_VALUE)
        {
            if (event.size < sizeof(clap_event_param_value_t)) return;
            const auto& value = reinterpret_cast<const clap_event_param_value_t&>(event);
            if (auto* state = stateFor(value.param_id)) state->setAutomatedBase(value.value);
            uiDirty.store(true, std::memory_order_release);
            host->request_callback(host);
        }
        else if (event.type == CLAP_EVENT_NOTE_ON || event.type == CLAP_EVENT_NOTE_OFF)
        {
            if (event.size < sizeof(clap_event_note_t)) return;
            const auto& note = reinterpret_cast<const clap_event_note_t&>(event);
            applyNote(event.type == CLAP_EVENT_NOTE_ON && note.velocity > 0.0,
                      note.key, static_cast<float>(note.velocity));
        }
        else if (event.type == CLAP_EVENT_MIDI)
        {
            if (event.size < sizeof(clap_event_midi_t)) return;
            const auto& midi = reinterpret_cast<const clap_event_midi_t&>(event);
            const auto status = midi.data[0] & 0xf0;
            if (status == 0x90 || status == 0x80)
                applyNote(status == 0x90 && midi.data[2] != 0, midi.data[1], midi.data[2] / 127.0f);
        }
    }

    void applyNote(bool on, int note, float velocity) noexcept
    {
        if (note < 0 || note > 127) return;
        if (on && std::isfinite(velocity))
        {
            std::array<double, parameters.size()> values {};
            for (size_t i = 0; i < states.size(); ++i)
            {
                (void) states[i].consumePublishedBase();
                values[i] = states[i].nextValue();
            }
            kick.trigger(values, velocity, note);
            ++visualFrame.hit;
            visualFrame.decayMs = static_cast<float>(kick.bodyDecayMs());
            visualFrame.punch = static_cast<float>(values[1] * 0.01);
            visualFrame.click = static_cast<float>(values[2] * 0.01);
            visualFrame.drive = static_cast<float>(values[3] / 12.0);
        }
    }

    template <typename Sample>
    void renderSamples(const char_clap::ProcessView& process, uint32_t begin, uint32_t end) noexcept
    {
        auto output = process.audioOutput<Sample>(0);
        auto* left = output.channel(0);
        auto* right = output.channel(1);
        if (!left || !right || begin >= end) return;


        for (uint32_t frame = begin; frame < end; ++frame)
        {
            const auto sample = static_cast<Sample>(kick.next());
            left[frame] = sample;
            right[frame] = sample;
            captureVisual(static_cast<float>(sample));
        }
    }

    void render(const char_clap::ProcessView& process, uint32_t begin, uint32_t end) noexcept
    {
        const auto& buffer = process.audioOutput<float>(0);
        if (buffer.channel(0) != nullptr) renderSamples<float>(process, begin, end);
        else renderSamples<double>(process, begin, end);
    }

    struct VisualFrame
    {
        uint32_t hit = 0;
        float energy = 0, peak = 0, edge = 0;
        float decayMs = 280;
        float punch = 0.5f, click = 0.3f, drive = 1.0f / 6.0f;
        std::array<std::array<float, 3>, 24> operators {};
    };

    void captureVisual(float sample) noexcept
    {
        visualFrame.operators[visualFrames * 24 / visualInterval] = kick.visualState();
        visualFrame.energy += sample * sample;
        visualFrame.peak = std::max(visualFrame.peak, std::abs(sample));
        visualFrame.edge += std::abs(sample - visualPrevious);
        visualPrevious = sample;
        if (++visualFrames < visualInterval) return;
        visualFrame.energy = std::sqrt(visualFrame.energy / visualFrames);
        visualFrame.edge /= visualFrames;
        // A slow/closed editor drops visual frames; audio never waits for it.
        (void) visualQueue.tryPush(visualFrame);
        visualFrames = 0;
        visualFrame.energy = visualFrame.peak = visualFrame.edge = 0;
    }

    VisualFrame visualFrame;
    clap::helpers::ParamQueue<VisualFrame, 4> visualQueue;
    uint32_t visualFrames = 0, visualInterval = 1600;
    float visualPrevious = 0;

    const clap_host_t* host;
    const clap_host_params_t* hostParams = nullptr;
    const clap_host_state_t* hostState = nullptr;
    const clap_host_preset_load_t* hostPresetLoad = nullptr;
    char_clap::WebUI ui;
    Kick kick;
    std::array<char_clap::ParameterState, parameters.size()> states {{
        { 20.0, 4000.0, 280.0 },
        { 0.0, 100.0, 50.0 }, { 0.0, 100.0, 30.0 }, { 0.0, 12.0, 2.0 }, { 0.0, 100.0, 0.0 }, { 0.25, 16.0, 2.6025 }, { 0.0, 1.0, 0.0 }, { 0.0, 100.0, 0.0 }
    }};
    clap::helpers::ParamQueue<Edit, 64> edits;
    clap::helpers::ParamQueue<UINote, 64> uiNotes;
    std::atomic<bool> uiDirty { true };
};

uint32_t pluginCount(const clap_plugin_factory_t*) { return 1; }
const clap_plugin_descriptor_t* pluginDescriptor(const clap_plugin_factory_t*, uint32_t index)
{
    return index == 0 ? &descriptor() : nullptr;
}
const clap_plugin_t* createPlugin(const clap_plugin_factory_t*, const clap_host_t* host, const char* id)
{
    if (!host || !id || std::strcmp(id, pluginId) != 0) return nullptr;
    return (new BDPlugin(host))->clapPlugin();
}

struct PresetProvider
{
    clap_preset_discovery_provider_t provider;
    const clap_preset_discovery_indexer_t* indexer;

    explicit PresetProvider(const clap_preset_discovery_indexer_t* indexer)
        : provider { &providerDescriptor(), this, init, destroy, metadata, extension }, indexer(indexer) {}

    static const clap_preset_discovery_provider_descriptor_t& providerDescriptor()
    {
        static const clap_preset_discovery_provider_descriptor_t value {
            CLAP_VERSION, "com.charlieculbert.bd.presets",
            "tapa Presets", "Charlie Culbert"
        };
        return value;
    }
    static PresetProvider& from(const clap_preset_discovery_provider_t* provider)
    {
        return *static_cast<PresetProvider*>(provider->provider_data);
    }
    static bool init(const clap_preset_discovery_provider_t* provider)
    {
        static const clap_preset_discovery_location_t location {
            CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, "Factory Presets",
            CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr
        };
        return from(provider).indexer->declare_location(from(provider).indexer, &location);
    }
    static void destroy(const clap_preset_discovery_provider_t* provider) { delete &from(provider); }
    static bool metadata(const clap_preset_discovery_provider_t*, uint32_t kind, const char* location,
                         const clap_preset_discovery_metadata_receiver_t* receiver)
    {
        if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || location || !receiver) return false;
        const clap_universal_plugin_id_t plugin { "clap", pluginId };
        for (const auto& preset : presets)
        {
            if (!receiver->begin_preset(receiver, preset.name, preset.key)) return false;
            receiver->add_plugin_id(receiver, &plugin);
            receiver->set_flags(receiver, CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT);
            receiver->add_creator(receiver, "Charlie Culbert");
            if (preset.suggestedNote >= 0 && receiver->set_description)
            {
                char description[160] {};
                std::snprintf(description, sizeof(description),
                              "Suggested MIDI note: %d. Play other notes to transpose; cymbals track more gently.",
                              preset.suggestedNote);
                receiver->set_description(receiver, description);
            }
            receiver->add_feature(receiver, CLAP_PLUGIN_FEATURE_INSTRUMENT);
        }
        return true;
    }
    static const void* extension(const clap_preset_discovery_provider_t*, const char*) { return nullptr; }
};

uint32_t presetProviderCount(const clap_preset_discovery_factory_t*) { return 1; }
const clap_preset_discovery_provider_descriptor_t* presetProviderDescriptor(
    const clap_preset_discovery_factory_t*, uint32_t index)
{
    return index == 0 ? &PresetProvider::providerDescriptor() : nullptr;
}
const clap_preset_discovery_provider_t* createPresetProvider(
    const clap_preset_discovery_factory_t*, const clap_preset_discovery_indexer_t* indexer,
    const char* id)
{
    if (!indexer || !id || std::strcmp(id, PresetProvider::providerDescriptor().id) != 0) return nullptr;
    return &(new PresetProvider(indexer))->provider;
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
        CLAP_PLUGIN_FEATURE_STEREO, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "tapa", "Charlie Culbert",
        "", "", "", "0.1.0", "FM drum with transient and sustained noise", features
    };
    return value;
}

bool entryInit(const char* path) { return char_clap::setResourceRoot(path); }
void entryDeinit()
{
    char_clap::resourceRoot.clear();
}

const void* entryGetFactory(const char* factoryId)
{
    if (!factoryId) return nullptr;
    if (std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0)
    {
        static const clap_plugin_factory_t factory { pluginCount, pluginDescriptor, createPlugin };
        return &factory;
    }
    if (std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID) == 0)
    {
        static const clap_preset_discovery_factory_t factory {
            presetProviderCount, presetProviderDescriptor, createPresetProvider
        };
        return &factory;
    }
    return nullptr;
}

} // namespace bd
