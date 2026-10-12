// Tapa: a monophonic FM drum with noise and metallic resonances, whose
// interface is a web page (resources/page).
//
// This file is the whole CLAP plugin: its descriptor, note and audio ports,
// parameters, state, presets, the clap.webview and clap.gui extensions its
// page uses, and the audio processing; DrumVoice.h is the sound itself and
// entry.cpp the entry point that makes the plugin.
#include "clap/clap.h"
#include "clap/ext/draft/webview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <type_traits>
#include "core/messages.h"
#include "core/resources.h"
#include "webview/gui.h"
#include "plugin.h"
#include "DrumVoice.h"
#include "Parameters.h"
#include "Presets.h"

using namespace tapa;

static_assert(std::atomic<double>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

using Values = std::array<double, parameters.size()>;

// Saved as raw bytes: version 6 is this, 72 bytes. Older versions hold fewer
// values, and still load (see extensionState).
struct State
{
    uint32_t magic = stateMagic, version = stateVersion;
    Values values{};
};
static_assert(sizeof(State) == 72);

// Parameter IDs are 1 to 8, each one more than its row in Parameters.h.
static const ParameterInfo *findParameter(clap_id id)
{
    return id >= 1 && id <= parameters.size() ? &parameters[id - 1] : nullptr;
}

static double clampParameter(const ParameterInfo &p, double value)
{
    return std::clamp(std::isfinite(value) ? value : p.minimum, p.minimum, p.maximum);
}

// What the page draws, measured on the audio thread about 30 times a second.
struct VisualFrame
{
    uint32_t hit = 0;
    float energy = 0, peak = 0, edge = 0;
    float decayMs = 280;
    float punch = 0.5f, click = 0.3f, drive = 1.0f / 6.0f;
    std::array<std::array<float, 3>, 24> operators{};
};

// Frames from the audio thread (the only writer) to the main thread (the only reader).
struct VisualQueue
{
    static constexpr uint32_t size = 4;
    VisualFrame frames[size];
    std::atomic<uint32_t> written{0}, read{0};

    bool push(const VisualFrame &frame)
    {
        const uint32_t w = written.load(std::memory_order_relaxed);
        if (w - read.load(std::memory_order_acquire) == size)
            return false;
        frames[w % size] = frame;
        written.store(w + 1, std::memory_order_release);
        return true;
    }

    bool pop(VisualFrame &frame)
    {
        const uint32_t r = read.load(std::memory_order_relaxed);
        if (r == written.load(std::memory_order_acquire))
            return false;
        frame = frames[r % size];
        read.store(r + 1, std::memory_order_release);
        return true;
    }
};

struct MyPlugin
{
    clap_plugin_t plugin;
    const clap_host_t *host;
    const clap_host_params_t *hostParams;
    const clap_host_preset_load_t *hostPresets;
    const clap_host_gui_t *hostGui; // request_resize, for the page's resize grip
    DrumVoice voice;

    // Set from the main thread (the page, state, presets) and the audio
    // thread (automation); the voice reads them at each hit.
    std::atomic<double> values[parameters.size()];
    std::atomic<bool> gestureBegin[parameters.size()], edited[parameters.size()], gestureEnd[parameters.size()];

    webview::Gui gui;
    bool guiSizesInPoints = true;
    double guiWidth = 640, guiHeight = 460; // points, exactly as set_size gave them, so get_size agrees
    std::atomic<bool> uiDirty{true};

    // Audio thread: the frame being measured.
    VisualFrame visualFrame;
    uint32_t visualFrames = 0, visualInterval = 1600;
    float visualPrevious = 0;
    VisualQueue visualQueue;
};

static void PluginApplyEvent(MyPlugin *plugin, const clap_event_header_t *event);
static void PluginSyncMainToAudio(MyPlugin *plugin, const clap_output_events_t *out);
static void PluginSetValue(MyPlugin *plugin, clap_id id, double value);
static void PluginNotifyValuesChanged(MyPlugin *plugin);
static bool PluginReceiveMessage(MyPlugin *plugin, const core::Value &message);

static const char *const pluginFeatures[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_SYNTHESIZER,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

static const clap_plugin_descriptor_t pluginDescriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id = pluginId,
    .name = "Tapa",
    .vendor = "Charlie Culbert",
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = "0.2.0",
    .description = "FM drum with transient and sustained noise",

    .features = pluginFeatures,
};

static constexpr clap_id outputPortId = 0x53594f55;
static constexpr clap_id notePortId = 0x42444b31;

static const clap_plugin_note_ports_t extensionNotePorts = {
    .count = [](const clap_plugin_t *plugin, bool isInput) -> uint32_t { return isInput ? 1 : 0; },

    .get = [](const clap_plugin_t *plugin, uint32_t index, bool isInput, clap_note_port_info_t *info) -> bool
    {
        if (!isInput || index)
            return false;
        *info = {};
        info->id = notePortId;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        snprintf(info->name, sizeof(info->name), "%s", "Notes");
        return true;
    },
};

static const clap_plugin_audio_ports_t extensionAudioPorts = {
    .count = [](const clap_plugin_t *plugin, bool isInput) -> uint32_t { return isInput ? 0 : 1; },

    .get = [](const clap_plugin_t *plugin, uint32_t index, bool isInput, clap_audio_port_info_t *info) -> bool
    {
        if (isInput || index)
            return false;
        *info = {};
        info->id = outputPortId;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN | CLAP_AUDIO_PORT_SUPPORTS_64BITS;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        snprintf(info->name, sizeof(info->name), "%s", "Stereo Output");
        return true;
    },
};

static const clap_plugin_params_t extensionParams = {
    .count = [](const clap_plugin_t *plugin) -> uint32_t { return (uint32_t)parameters.size(); },

    .get_info = [](const clap_plugin_t *_plugin, uint32_t index, clap_param_info_t *info) -> bool
    {
        if (index >= parameters.size())
            return false;
        const auto &p = parameters[index];
        *info = {};
        info->id = p.id;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE;
        if (p.id == 7) // Linked Decay: Body or Linked
            info->flags |= CLAP_PARAM_IS_STEPPED;
        info->min_value = p.minimum;
        info->max_value = p.maximum;
        info->default_value = p.defaultValue;
        snprintf(info->name, sizeof(info->name), "%s", p.name);
        return true;
    },

    .get_value = [](const clap_plugin_t *_plugin, clap_id id, double *value) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        if (!findParameter(id))
            return false;
        *value = plugin->values[id - 1].load(std::memory_order_relaxed);
        return true;
    },

    .value_to_text = [](const clap_plugin_t *_plugin, clap_id id, double value, char *text, uint32_t size) -> bool
    {
        const auto *p = findParameter(id);
        if (!p || !size)
            return false;
        if (id == 7)
            return snprintf(text, size, "%s", value >= .5 ? "Linked" : "Body") > 0;
        return snprintf(text, size, "%.2f %s", value, p->unit) > 0;
    },

    .text_to_value = [](const clap_plugin_t *_plugin, clap_id id, const char *text, double *value) -> bool
    {
        const auto *p = findParameter(id);
        if (!p)
            return false;
        if (id == 7 && (!strcmp(text, "Body") || !strcmp(text, "Linked")))
        {
            *value = !strcmp(text, "Linked") ? 1.0 : 0.0;
            return true;
        }
        char *end = nullptr;
        const double parsed = strtod(text, &end); // "280 ms" reads as 280
        if (end == text || !std::isfinite(parsed))
            return false;
        *value = clampParameter(*p, parsed);
        return true;
    },

    .flush = [](const clap_plugin_t *_plugin, const clap_input_events_t *in, const clap_output_events_t *out)
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        PluginSyncMainToAudio(plugin, out);
        const uint32_t eventCount = in ? in->size(in) : 0;
        for (uint32_t i = 0; i < eventCount; i++)
            if (const auto *event = in->get(in, i))
                PluginApplyEvent(plugin, event);
    },
};

// The oversampler's downsampling delays the output by a fixed few samples.
static const clap_plugin_latency_t extensionLatency = {
    .get = [](const clap_plugin_t *_plugin) -> uint32_t
    { return ((MyPlugin *)_plugin->plugin_data)->voice.latency(); },
};

// Hosts may show a parameter's controller mapping or automation state; the
// page shows neither yet.
static const clap_plugin_param_indication_t extensionParamIndication = {
    .set_mapping = [](const clap_plugin_t *_plugin, clap_id paramId, bool hasMapping, const clap_color_t *color,
                      const char *label, const char *description) {},

    .set_automation = [](const clap_plugin_t *_plugin, clap_id paramId, uint32_t automationState,
                         const clap_color_t *color) {},
};

static bool writeAll(const clap_ostream_t *stream, const void *data, uint64_t size)
{
    const char *bytes = (const char *)data;
    while (size)
    {
        const int64_t written = stream->write(stream, bytes, size);
        if (written <= 0)
            return false;
        bytes += written;
        size -= (uint64_t)written;
    }
    return true;
}

// Reads exactly size bytes, however few each read returns.
static bool readAll(const clap_istream_t *stream, void *data, uint64_t size)
{
    unsigned char *bytes = (unsigned char *)data;
    while (size)
    {
        const int64_t read = stream->read(stream, bytes, size);
        if (read <= 0 || (uint64_t)read > size)
            return false;
        bytes += read;
        size -= (uint64_t)read;
    }
    return true;
}

static const clap_plugin_state_t extensionState = {
    .save = [](const clap_plugin_t *_plugin, const clap_ostream_t *stream) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        State state;
        for (size_t i = 0; i < parameters.size(); i++)
            state.values[i] = plugin->values[i].load(std::memory_order_relaxed);
        return writeAll(stream, &state, sizeof(state));
    },

    .load = [](const clap_plugin_t *_plugin, const clap_istream_t *stream) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        State state;
        if (!readAll(stream, &state, 2 * sizeof(uint32_t)) || state.magic != stateMagic)
            return false;
        state.values = {};

        // Each older version saved fewer values, in the same order, and a
        // missing value loads as 0. Version 1 began with one more, now unused.
        if (state.version == 1)
        {
            std::array<double, 5> legacy{};
            if (!readAll(stream, legacy.data(), sizeof(legacy)))
                return false;
            std::copy(legacy.begin() + 1, legacy.end(), state.values.begin());
        }
        else if (state.version >= 2 && state.version <= 6)
        {
            const size_t count = state.version == 2 ? 4 : state.version + 2;
            if (!readAll(stream, state.values.data(), count * sizeof(double)))
                return false;
        }
        else
            return false;
        // Before version 4 the FM ratio followed FM Amount.
        if (state.version < 4)
        {
            const double amount = std::isfinite(state.values[1]) ? std::clamp(state.values[1], 0.0, 100.0) * .01 : .5;
            state.values[5] = 1.0 + 6.41 * amount * amount;
        }
        for (const auto &p : parameters)
            PluginSetValue(plugin, p.id, state.values[p.id - 1]);
        PluginNotifyValuesChanged(plugin);
        return true;
    },
};

static const clap_plugin_preset_load_t extensionPresetLoad = {
    .from_location = [](const clap_plugin_t *_plugin, uint32_t kind, const char *location, const char *key) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || location || !key)
            return false;
        for (const auto &preset : presets)
            if (!strcmp(preset.key, key))
            {
                for (const auto &p : parameters)
                    PluginSetValue(plugin, p.id, preset.values[p.id - 1]);
                PluginNotifyValuesChanged(plugin);
                if (plugin->hostPresets)
                    plugin->hostPresets->loaded(plugin->host, kind, location, key);
                return true;
            }
        return false;
    },
};

static const clap_plugin_webview_t extensionWebview = {
    .get_uri = [](const clap_plugin_t *_plugin, char *uri, uint32_t capacity) -> int32_t
    {
        static const char start[] = "/page/index.html";
        if (capacity)
            snprintf(uri, capacity, "%s", start);
        return sizeof(start); // including the terminating zero
    },

    .get_resource = [](const clap_plugin_t *_plugin, const char *path, char *mime, uint32_t mimeCapacity,
                       const clap_ostream_t *stream) -> bool
    {
        auto resource = core::readResource(path);
        if (!resource || resource->mime.size() >= mimeCapacity)
            return false;
        strcpy(mime, resource->mime.c_str());
        return writeAll(stream, resource->bytes.data(), resource->bytes.size());
    },

    .receive = [](const clap_plugin_t *_plugin, const void *buffer, uint32_t size) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        auto message = core::decode(buffer, size);
        return message && PluginReceiveMessage(plugin, *message);
    },
};

// clap.gui sizes are points on macOS and pixels elsewhere, where a display
// scaled to 150% needs 1.5 pixels per point.
static double PluginPixelsPerPoint(const MyPlugin *plugin)
{
    return plugin->guiSizesInPoints ? 1 : plugin->gui.pixelsPerPoint();
}

static uint32_t PluginToHost(const MyPlugin *plugin, double points)
{
    return (uint32_t)std::lround(points * PluginPixelsPerPoint(plugin));
}

// The page fills any window, so any size above zero will do: an AUv3 passes
// on whatever bounds its host gives, small ones included.
static const clap_plugin_gui_t extensionGui = {
    .is_api_supported = [](const clap_plugin_t *_plugin, const char *api, bool isFloating) -> bool
    { return ((MyPlugin *)_plugin->plugin_data)->gui.isApiSupported(api, isFloating); },

    .get_preferred_api = [](const clap_plugin_t *_plugin, const char **api, bool *isFloating) -> bool
    { return ((MyPlugin *)_plugin->plugin_data)->gui.getPreferredApi(api, isFloating); },

    .create = [](const clap_plugin_t *_plugin, const char *api, bool isFloating) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        if (!plugin->gui.create(api, isFloating))
            return false;
        plugin->guiSizesInPoints = strcmp(api, CLAP_WINDOW_API_WIN32) != 0 && strcmp(api, CLAP_WINDOW_API_X11) != 0;
        plugin->gui.setSize(PluginToHost(plugin, plugin->guiWidth), PluginToHost(plugin, plugin->guiHeight));
        return true;
    },

    .destroy = [](const clap_plugin_t *_plugin)
    { ((MyPlugin *)_plugin->plugin_data)->gui.destroy(); },

    .set_scale = [](const clap_plugin_t *_plugin, double scale) -> bool
    { return false; },

    .get_size = [](const clap_plugin_t *_plugin, uint32_t *width, uint32_t *height) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        *width = PluginToHost(plugin, plugin->guiWidth);
        *height = PluginToHost(plugin, plugin->guiHeight);
        return true;
    },

    .can_resize = [](const clap_plugin_t *_plugin) -> bool
    { return true; },

    .get_resize_hints = [](const clap_plugin_t *_plugin, clap_gui_resize_hints_t *hints) -> bool
    {
        hints->can_resize_horizontally = true;
        hints->can_resize_vertically = true;
        hints->preserve_aspect_ratio = false;
        return true;
    },

    .adjust_size = [](const clap_plugin_t *_plugin, uint32_t *width, uint32_t *height) -> bool
    { return width && height && *width > 0 && *height > 0; },

    .set_size = [](const clap_plugin_t *_plugin, uint32_t width, uint32_t height) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        if (!width || !height)
            return false;
        const double scale = PluginPixelsPerPoint(plugin);
        plugin->guiWidth = width / scale;
        plugin->guiHeight = height / scale;
        plugin->gui.setSize(width, height);
        return true;
    },

    .set_parent = [](const clap_plugin_t *_plugin, const clap_window_t *window) -> bool
    { return ((MyPlugin *)_plugin->plugin_data)->gui.setParent(window); },

    .set_transient = [](const clap_plugin_t *_plugin, const clap_window_t *window) -> bool
    { return false; },

    .suggest_title = [](const clap_plugin_t *_plugin, const char *title) {},

    .show = [](const clap_plugin_t *_plugin) -> bool
    { return ((MyPlugin *)_plugin->plugin_data)->gui.show(); },

    .hide = [](const clap_plugin_t *_plugin) -> bool
    { return ((MyPlugin *)_plugin->plugin_data)->gui.hide(); },
};

static const clap_plugin_timer_support_t extensionTimerSupport = {
    .on_timer = [](const clap_plugin_t *_plugin, clap_id timerId)
    { ((MyPlugin *)_plugin->plugin_data)->gui.onTimer(timerId); },
};

static void PluginSendValues(MyPlugin *plugin)
{
    core::Value::Map values;
    for (const auto &p : parameters)
        values[std::to_string(p.id)] = plugin->values[p.id - 1].load(std::memory_order_relaxed);
    plugin->gui.send(core::Value::Map{{"type", "values"}, {"values", values}});
}

// Sends the newest frame the audio thread has measured since the page last
// asked, if there is one.
static void PluginSendVisual(MyPlugin *plugin)
{
    VisualFrame frame;
    bool any = false;
    while (plugin->visualQueue.pop(frame))
        any = true;
    if (!any)
        return;
    core::Value::Array operators;
    for (const auto &snapshot : frame.operators)
        for (float value : snapshot)
            operators.push_back((double)value);
    plugin->gui.send(core::Value::Map{
        {"type", "visual"}, {"hit", (double)frame.hit}, {"energy", (double)frame.energy},
        {"peak", (double)frame.peak}, {"edge", (double)frame.edge}, {"decayMs", (double)frame.decayMs},
        {"punch", (double)frame.punch}, {"click", (double)frame.click}, {"drive", (double)frame.drive},
        {"operators", operators}});
}

// Messages between the page and the plugin (main thread only):
//   page -> plugin  {type: "ready"}
//                   {type: "begin" | "end", id}     a drag starts or ends
//                   {type: "change", id, value}
//                   {type: "visual"}                the page polls for the newest frame
//                   {type: "resize", width, height, scale}  the page's grip (CSS pixels)
//   plugin -> page  {type: "values", values: {id: value}}
//                   {type: "visual", hit, energy, peak, edge, decayMs, punch, click, drive,
//                    operators: [72 numbers: 24 snapshots of 3]}
static bool PluginReceiveMessage(MyPlugin *plugin, const core::Value &message)
{
    const auto type = message["type"].text();
    if (type == "ready")
    {
        PluginSendValues(plugin);
        return true;
    }

    if (type == "visual")
    {
        PluginSendVisual(plugin);
        return true;
    }

    if (type == "resize")
    {
        // The page's grip: ask the host for the new size (it then calls
        // set_size). CSS pixels are points on macOS.
        const double perPixel = plugin->guiSizesInPoints ? 1 : message["scale"].number(1);
        uint32_t width = (uint32_t)std::max(1.0, message["width"].number(0) * perPixel);
        uint32_t height = (uint32_t)std::max(1.0, message["height"].number(0) * perPixel);
        // A host that agrees needn't call set_size afterwards: take the size then.
        if (plugin->hostGui && (width != PluginToHost(plugin, plugin->guiWidth) || height != PluginToHost(plugin, plugin->guiHeight)) &&
            plugin->hostGui->request_resize(plugin->host, width, height))
            extensionGui.set_size(&plugin->plugin, width, height);
        return true;
    }

    const double number = message["id"].number(-1);
    if (!(number == std::floor(number) && findParameter((clap_id)number)))
        return false;
    const clap_id id = (clap_id)number;

    if (type == "change")
    {
        const double value = message["value"].number(NAN);
        if (!std::isfinite(value))
            return false;
        PluginSetValue(plugin, id, value);
        plugin->edited[id - 1].store(true, std::memory_order_release);
    }
    else if (type == "begin")
        plugin->gestureBegin[id - 1].store(true, std::memory_order_release);
    else if (type == "end")
        plugin->gestureEnd[id - 1].store(true, std::memory_order_release);
    else
        return false;

    // The host then calls process() or flush(), which send these to the host
    // as gesture and value events.
    if (plugin->hostParams)
        plugin->hostParams->request_flush(plugin->host);
    plugin->host->request_process(plugin->host);
    return true;
}

static void PluginSetValue(MyPlugin *plugin, clap_id id, double value)
{
    plugin->values[id - 1].store(clampParameter(*findParameter(id), value), std::memory_order_relaxed);
}

// After a state or preset load: the host and the page re-read every value.
static void PluginNotifyValuesChanged(MyPlugin *plugin)
{
    plugin->uiDirty.store(true, std::memory_order_release);
    if (plugin->hostParams)
        plugin->hostParams->rescan(plugin->host, CLAP_PARAM_RESCAN_VALUES);
    plugin->host->request_callback(plugin->host);
}

static void PluginNoteOn(MyPlugin *plugin, int key, float velocity)
{
    if (key < 0 || key > 127 || !std::isfinite(velocity))
        return;
    Values values;
    for (size_t i = 0; i < parameters.size(); i++)
        values[i] = plugin->values[i].load(std::memory_order_relaxed);
    plugin->voice.trigger(values, velocity, key);
    auto &frame = plugin->visualFrame;
    ++frame.hit;
    frame.decayMs = (float)plugin->voice.bodyDecayMs();
    frame.punch = (float)(values[1] * 0.01);
    frame.click = (float)(values[2] * 0.01);
    frame.drive = (float)(values[3] / 12.0);
}

// Note-offs do nothing: a hit always plays out its tail.
static void PluginApplyEvent(MyPlugin *plugin, const clap_event_header_t *event)
{
    if (event->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;
    if (event->type == CLAP_EVENT_NOTE_ON && event->size >= sizeof(clap_event_note_t))
    {
        const auto *note = (const clap_event_note_t *)event;
        if (note->velocity > 0)
            PluginNoteOn(plugin, note->key, (float)note->velocity);
    }
    else if (event->type == CLAP_EVENT_MIDI && event->size >= sizeof(clap_event_midi_t))
    {
        const uint8_t *data = ((const clap_event_midi_t *)event)->data;
        if ((data[0] & 0xF0) == 0x90 && data[2] > 0)
            PluginNoteOn(plugin, data[1], data[2] / 127.0f);
    }
    else if (event->type == CLAP_EVENT_PARAM_VALUE && event->size >= sizeof(clap_event_param_value_t))
    {
        const auto *p = (const clap_event_param_value_t *)event;
        if (!findParameter(p->param_id))
            return;
        PluginSetValue(plugin, p->param_id, p->value);
        // Tell the page on the main thread (see on_main_thread).
        if (!plugin->uiDirty.exchange(true, std::memory_order_acq_rel))
            plugin->host->request_callback(plugin->host);
    }
}

static void PluginSendGesture(const clap_output_events_t *out, clap_id id, uint16_t type)
{
    clap_event_param_gesture_t event = {};
    event.header.size = sizeof(event);
    event.header.time = 0;
    event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    event.header.type = type;
    event.header.flags = CLAP_EVENT_IS_LIVE;
    event.param_id = id;
    out->try_push(out, &event.header);
}

// Sends the page's edits to the host: per parameter, begin, then the value, then end.
static void PluginSyncMainToAudio(MyPlugin *plugin, const clap_output_events_t *out)
{
    if (!out)
        return;
    for (const auto &p : parameters)
    {
        const size_t i = p.id - 1;
        if (plugin->gestureBegin[i].exchange(false, std::memory_order_acquire))
            PluginSendGesture(out, p.id, CLAP_EVENT_PARAM_GESTURE_BEGIN);

        if (plugin->edited[i].exchange(false, std::memory_order_acquire))
        {
            clap_event_param_value_t event = {};
            event.header.size = sizeof(event);
            event.header.time = 0;
            event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            event.header.type = CLAP_EVENT_PARAM_VALUE;
            event.header.flags = CLAP_EVENT_IS_LIVE;
            event.param_id = p.id;
            event.cookie = NULL;
            event.note_id = -1;
            event.port_index = -1;
            event.channel = -1;
            event.key = -1;
            event.value = plugin->values[i].load(std::memory_order_relaxed);
            out->try_push(out, &event.header);
        }

        if (plugin->gestureEnd[i].exchange(false, std::memory_order_acquire))
            PluginSendGesture(out, p.id, CLAP_EVENT_PARAM_GESTURE_END);
    }
}

// Measures one output sample for the page: level, peak, roughness and the
// voice's operators, handed over once per frame (about 1/30 s).
static void PluginCaptureVisual(MyPlugin *plugin, float sample)
{
    auto &frame = plugin->visualFrame;
    frame.operators[plugin->visualFrames * 24 / plugin->visualInterval] = plugin->voice.visualState();
    frame.energy += sample * sample;
    frame.peak = std::max(frame.peak, std::abs(sample));
    frame.edge += std::abs(sample - plugin->visualPrevious);
    plugin->visualPrevious = sample;
    if (++plugin->visualFrames < plugin->visualInterval)
        return;
    frame.energy = std::sqrt(frame.energy / plugin->visualFrames);
    frame.edge /= plugin->visualFrames;
    // A slow or closed page drops frames; audio never waits for it.
    plugin->visualQueue.push(frame);
    plugin->visualFrames = 0;
    frame.energy = frame.peak = frame.edge = 0;
}

template <typename Sample>
static Sample *PluginChannel(const clap_audio_buffer_t &buffer, uint32_t index)
{
    if (index >= buffer.channel_count)
        return nullptr;
    if constexpr (std::is_same_v<Sample, float>)
        return buffer.data32 ? buffer.data32[index] : nullptr;
    else
        return buffer.data64 ? buffer.data64[index] : nullptr;
}

template <typename Sample>
static void PluginRenderAudio(MyPlugin *plugin, const clap_audio_buffer_t &output, uint32_t start, uint32_t end)
{
    Sample *left = PluginChannel<Sample>(output, 0), *right = PluginChannel<Sample>(output, 1);
    if (!left || !right)
        return;
    for (uint32_t i = start; i < end; i++)
    {
        const Sample sample = (Sample)plugin->voice.next();
        left[i] = right[i] = sample;
        PluginCaptureVisual(plugin, (float)sample);
    }
}

static void PluginRender(MyPlugin *plugin, const clap_process_t *process, uint32_t start, uint32_t end)
{
    if (end <= start)
        return;
    const auto &output = process->audio_outputs[0];
    if (PluginChannel<float>(output, 0))
        PluginRenderAudio<float>(plugin, output, start, end);
    else
        PluginRenderAudio<double>(plugin, output, start, end);
}

static const clap_plugin_t pluginClass = {
    .desc = &pluginDescriptor,
    .plugin_data = nullptr,
    .init = [](const clap_plugin *_plugin) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        const clap_host_t *host = plugin->host;
        plugin->hostParams = (const clap_host_params_t *)host->get_extension(host, CLAP_EXT_PARAMS);
        plugin->hostPresets = (const clap_host_preset_load_t *)host->get_extension(host, CLAP_EXT_PRESET_LOAD);
        plugin->hostGui = (const clap_host_gui_t *)host->get_extension(host, CLAP_EXT_GUI);
        plugin->gui.init(_plugin, host);
        return true;
    },
    .destroy = [](const clap_plugin *_plugin)
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        plugin->gui.destroy();
        delete plugin;
    },
    .activate = [](const clap_plugin *_plugin, double sampleRate, uint32_t minimumFramesCount, uint32_t maximumFramesCount) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        if (!(sampleRate > 0))
            return false;
        plugin->voice.prepare(sampleRate);
        plugin->visualInterval = std::max(1u, (uint32_t)(sampleRate / 30.0));
        plugin->visualFrame = {};
        plugin->visualFrames = 0;
        plugin->visualPrevious = 0;
        return true;
    },
    .deactivate = [](const clap_plugin *_plugin) {},

    .start_processing = [](const clap_plugin *_plugin) -> bool
    {
        return true;
    },

    .stop_processing = [](const clap_plugin *_plugin) {},

    .reset = [](const clap_plugin *_plugin)
    {
        ((MyPlugin *)_plugin->plugin_data)->voice.reset();
    },

    .process = [](const clap_plugin *_plugin, const clap_process_t *process) -> clap_process_status
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        if (process->audio_outputs_count == 0)
            return CLAP_PROCESS_ERROR;

        PluginSyncMainToAudio(plugin, process->out_events);

        const uint32_t frameCount = process->frames_count;
        const clap_input_events_t *in = process->in_events;
        const uint32_t eventCount = in ? in->size(in) : 0;
        uint32_t frame = 0;
        for (uint32_t i = 0; i < eventCount; i++)
        {
            const clap_event_header_t *event = in->get(in, i);
            if (!event)
                continue;
            const uint32_t time = std::min(event->time, frameCount);
            if (time > frame)
            {
                PluginRender(plugin, process, frame, time);
                frame = time;
            }
            PluginApplyEvent(plugin, event);
        }
        PluginRender(plugin, process, frame, frameCount);
        return CLAP_PROCESS_CONTINUE;
    },

    .get_extension = [](const clap_plugin *plugin, const char *id) -> const void *
    {
        if (0 == strcmp(id, CLAP_EXT_NOTE_PORTS))
            return &extensionNotePorts;
        if (0 == strcmp(id, CLAP_EXT_AUDIO_PORTS))
            return &extensionAudioPorts;
        if (0 == strcmp(id, CLAP_EXT_PARAMS))
            return &extensionParams;
        if (0 == strcmp(id, CLAP_EXT_LATENCY))
            return &extensionLatency;
        if (0 == strcmp(id, CLAP_EXT_PARAM_INDICATION))
            return &extensionParamIndication;
        if (0 == strcmp(id, CLAP_EXT_STATE))
            return &extensionState;
        if (0 == strcmp(id, CLAP_EXT_PRESET_LOAD))
            return &extensionPresetLoad;
        if (0 == strcmp(id, CLAP_EXT_WEBVIEW))
            return &extensionWebview;
        if (0 == strcmp(id, CLAP_EXT_GUI))
            return &extensionGui;
        if (0 == strcmp(id, CLAP_EXT_TIMER_SUPPORT))
            return &extensionTimerSupport;
        return nullptr;
    },

    .on_main_thread = [](const clap_plugin *_plugin)
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        if (plugin->uiDirty.exchange(false, std::memory_order_acq_rel))
            PluginSendValues(plugin);
    },
};

static const clap_preset_discovery_provider_descriptor_t presetProviderDescriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "com.charlieculbert.tapa.presets",
    .name = "Tapa Presets",
    .vendor = "Charlie Culbert",
};

struct PresetProvider
{
    clap_preset_discovery_provider_t provider;
    const clap_preset_discovery_indexer_t *indexer;
};

static const clap_preset_discovery_provider_t presetProviderClass = {
    .desc = &presetProviderDescriptor,
    .provider_data = nullptr,

    .init = [](const clap_preset_discovery_provider_t *provider) -> bool
    {
        static const clap_preset_discovery_location_t location = {
            .flags = CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT,
            .name = "Factory Presets",
            .kind = CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN,
            .location = nullptr,
        };
        const auto *indexer = ((PresetProvider *)provider->provider_data)->indexer;
        return indexer->declare_location(indexer, &location);
    },

    .destroy = [](const clap_preset_discovery_provider_t *provider)
    { delete (PresetProvider *)provider->provider_data; },

    .get_metadata = [](const clap_preset_discovery_provider_t *provider, uint32_t kind, const char *location,
                       const clap_preset_discovery_metadata_receiver_t *receiver) -> bool
    {
        if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || location || !receiver)
            return false;
        const clap_universal_plugin_id_t plugin = {.abi = "clap", .id = pluginId};
        for (const auto &preset : presets)
        {
            if (!receiver->begin_preset(receiver, preset.name, preset.key))
                return false;
            receiver->add_plugin_id(receiver, &plugin);
            receiver->set_flags(receiver, CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT);
            receiver->add_creator(receiver, "Charlie Culbert");
            if (preset.suggestedNote >= 0 && receiver->set_description)
            {
                char description[160] = {};
                snprintf(description, sizeof(description),
                         "Suggested MIDI note: %d. Play other notes to transpose; cymbals track more gently.",
                         preset.suggestedNote);
                receiver->set_description(receiver, description);
            }
            receiver->add_feature(receiver, CLAP_PLUGIN_FEATURE_INSTRUMENT);
        }
        return true;
    },

    .get_extension = [](const clap_preset_discovery_provider_t *provider, const char *id) -> const void *
    { return nullptr; },
};

static const clap_preset_discovery_factory_t presetDiscoveryFactory = {
    .count = [](const clap_preset_discovery_factory_t *factory) -> uint32_t { return 1; },

    .get_descriptor = [](const clap_preset_discovery_factory_t *factory, uint32_t index)
        -> const clap_preset_discovery_provider_descriptor_t *
    { return index == 0 ? &presetProviderDescriptor : nullptr; },

    .create = [](const clap_preset_discovery_factory_t *factory, const clap_preset_discovery_indexer_t *indexer,
                 const char *id) -> const clap_preset_discovery_provider_t *
    {
        if (!indexer || !id || strcmp(id, presetProviderDescriptor.id))
            return nullptr;
        PresetProvider *provider = new PresetProvider{presetProviderClass, indexer};
        provider->provider.provider_data = provider;
        return &provider->provider;
    },
};

const clap_plugin_descriptor_t *getPluginDescriptor()
{
    return &pluginDescriptor;
}

const clap_preset_discovery_factory_t *getPresetDiscoveryFactory()
{
    return &presetDiscoveryFactory;
}

const clap_plugin_t *createPlugin(const clap_host_t *host)
{
    MyPlugin *plugin = new MyPlugin();
    plugin->host = host;
    plugin->plugin = pluginClass;
    plugin->plugin.plugin_data = plugin;
    for (const auto &p : parameters)
        plugin->values[p.id - 1].store(p.defaultValue, std::memory_order_relaxed);
    return &plugin->plugin;
}
