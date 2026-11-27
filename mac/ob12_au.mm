// OB-12 as an Audio Unit v2 music device (aumu) for macOS: stereo out, MIDI in, the panel as a
// Cocoa view. Written against AudioToolbox alone: no Apple AU SDK classes, no framework. The machine
// is the same engine as the VST3's; the feeding thread, ring and resampler are the VST3's too.
#import <AudioToolbox/AudioToolbox.h>
#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/qos.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "ob12_engine.h"
#include "panel_view.h"
#include "ob12_mac.h"

#ifndef OB12_VERSION_INT
#define OB12_VERSION_INT 0x000A00  // 0.10.0, as the release it is built from
#endif
#ifndef OB12_VIEW_FACTORY
#define OB12_VIEW_FACTORY OB12_AUViewFactory_0A00
#endif
#define OB12_STR2(x) #x
#define OB12_STR(x) OB12_STR2(x)

namespace {

const int ring_size = 1 << 15;  // frames, a power of two
const int lead_frames = 2304;   // 48 ms in the ring

std::string bundle_path() {  // .../OB12.component
    Dl_info info;
    if (!dladdr((const void*)&bundle_path, &info) || !info.dli_fname) return "";
    std::string binary = info.dli_fname;  // .../OB12.component/Contents/MacOS/OB12
    for (int i = 0; i < 3; i++) binary = binary.substr(0, binary.find_last_of('/'));
    return binary;
}

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// See ob12_vst3.cpp: PCM1716 DAC, gain 2.375 inverting stage, two DC blocks.
struct AnalogOut {
    static constexpr float gain = 47.5f / 20.0f;
    float x1[2] = {}, y1[2] = {}, x2[2] = {}, y2[2] = {};
    float a1 = 0, a2 = 0;
    AnalogOut() {
        a1 = 1.f / (1.f + 6.2831853f * (1.f / (6.2831853f * 10e-6f * 47e3f)) / 48000.f);
        a2 = 1.f / (1.f + 6.2831853f * (1.f / (6.2831853f * 10e-6f * 10e3f)) / 48000.f);
    }
    float run(int c, float x) {
        float h1 = a1 * (y1[c] + x - x1[c]); x1[c] = x; y1[c] = h1;
        float g = h1 * gain;
        float h2 = a2 * (y2[c] + g - x2[c]); x2[c] = g; y2[c] = h2;
        return h2;
    }
};

// The machine and its feeding thread, as in the VST3.
struct Instrument {
    std::unique_ptr<Engine> engine;
    std::thread feeder;
    std::atomic<bool> stop{false};
    std::vector<float> ring = std::vector<float>(ring_size * 2);
    std::atomic<uint32_t> written{0}, read{0};
    std::atomic<long> underruns{0};
    AnalogOut analog;
    double position = 0, step = 1;
    float last[2] = {}, next[2] = {};

    bool start() {
        std::string rom = ob12mac::rom_folder();
        if (!exists(rom + "/12cpu152-CPU-v1.52-FINAL.bin")) return false;
        try {
            engine.reset(new Engine(rom));
        } catch (...) {
            return false;
        }
        engine->set_flash_file(ob12mac::support_folder(true) + "/flash.bin");
        feeder = std::thread([this]() {
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            pthread_setname_np("OB-12 engine");
            std::vector<int32_t> block(240 * 2);
            while (!stop) {
                uint32_t fill = written.load(std::memory_order_relaxed) - read.load(std::memory_order_acquire);
                bool ready = engine->sounding_ready();
                if (ready && fill >= (uint32_t)lead_frames) { usleep(1000); continue; }
                try {
                    engine->run(block.data(), 240);
                } catch (...) {
                    stop = true;
                    return;  // the firmware hit something unmodelled: silence, not a crashed host
                }
                if (!ready) continue;
                uint32_t w = written.load(std::memory_order_relaxed);
                const float gain = PanelView::output_gain();
                for (int i = 0; i < 240; i++, w++) {
                    ring[(w & (ring_size - 1)) * 2] = analog.run(0, block[i * 2] / 8388608.f) * gain;
                    ring[(w & (ring_size - 1)) * 2 + 1] = analog.run(1, block[i * 2 + 1] / 8388608.f) * gain;
                }
                written.store(w, std::memory_order_release);
            }
        });
        return true;
    }
    ~Instrument() {
        stop = true;
        if (feeder.joinable()) feeder.join();
    }
    bool pull() {
        uint32_t r = read.load(std::memory_order_relaxed);
        if (written.load(std::memory_order_acquire) == r) return false;
        last[0] = next[0];
        last[1] = next[1];
        next[0] = ring[(r & (ring_size - 1)) * 2];
        next[1] = ring[(r & (ring_size - 1)) * 2 + 1];
        read.store(r + 1, std::memory_order_release);
        return true;
    }
    void render(float* l, float* r, int n, bool wait) {
        for (int i = 0; i < n; i++) {
            position += step;
            bool held = false;
            while (position >= 1) {
                while (!pull()) {
                    if (wait && engine->sounding_ready() && !stop) { usleep(1000); continue; }
                    held = true;
                    break;
                }
                if (held) break;
                position -= 1;
            }
            if (held) { underruns++; position = 0; l[i] = 0; if (r) r[i] = 0; continue; }
            float a = (float)position;
            l[i] = last[0] + (next[0] - last[0]) * a;
            if (r) r[i] = last[1] + (next[1] - last[1]) * a;
        }
    }
};

struct Listener {
    AudioUnitPropertyID id;
    AudioUnitPropertyListenerProc proc;
    void* data;
};
struct Notify {
    AURenderCallback proc;
    void* data;
};

// The component instance. The plug-in interface must be its first member: the system calls our
// functions with the address of that interface as self.
struct Plugin {
    AudioComponentPlugInInterface face;
    AudioComponentInstance instance = nullptr;
    AudioComponentDescription description;

    bool initialized = false;
    Float64 rate = 48000;
    UInt32 channels = 2;
    UInt32 max_frames = 4096;
    UInt32 offline = 0;
    UInt32 allocate = 1;
    OSStatus last_error = noErr;
    std::vector<float> own[2];
    std::string preset_name = "Untitled";
    SInt32 preset_number = -1;
    HostCallbackInfo host = {};

    std::mutex instrument_lock;
    std::shared_ptr<Instrument> instrument;
    std::atomic<Instrument*> live{nullptr};  // the same, for the audio thread
    std::vector<Listener> listeners;
    std::mutex listener_lock;  // listeners are added and removed while properties change
    std::vector<Notify> notifies;
    std::mutex notify_lock;

    std::shared_ptr<Instrument> ensure() {
        std::lock_guard<std::mutex> hold(instrument_lock);
        if (!instrument) {
            auto fresh = std::make_shared<Instrument>();
            if (fresh->start()) { instrument = fresh; live.store(fresh.get(), std::memory_order_release); }
        }
        return instrument;
    }

    void changed(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element) {
        std::vector<Listener> copy;
        {
            std::lock_guard<std::mutex> hold(listener_lock);
            copy = listeners;
        }
        for (auto& l : copy) if (l.id == id) l.proc(l.data, instance, id, scope, element);
    }

    AudioStreamBasicDescription format() const {
        AudioStreamBasicDescription f = {};
        f.mSampleRate = rate;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
        f.mBytesPerPacket = 4;
        f.mFramesPerPacket = 1;
        f.mBytesPerFrame = 4;
        f.mChannelsPerFrame = channels;
        f.mBitsPerChannel = 32;
        return f;
    }
};

Plugin* me(void* self) { return (Plugin*)self; }

// ---------------------------------------------------------------- properties

OSStatus property_info(Plugin* p, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element, UInt32& size, Boolean& writable) {
    writable = false;
    switch (id) {
    case kAudioUnitProperty_ClassInfo:
        if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        size = sizeof(CFPropertyListRef); writable = true; return noErr;
    case kAudioUnitProperty_StreamFormat:
        if (scope == kAudioUnitScope_Input) return kAudioUnitErr_InvalidElement;
        if (scope != kAudioUnitScope_Output) return kAudioUnitErr_InvalidScope;
        if (element != 0) return kAudioUnitErr_InvalidElement;
        size = sizeof(AudioStreamBasicDescription); writable = true; return noErr;
    case kAudioUnitProperty_SampleRate:
        if (scope == kAudioUnitScope_Input) return kAudioUnitErr_InvalidElement;
        if (scope != kAudioUnitScope_Output && scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        size = sizeof(Float64); writable = true; return noErr;
    case kAudioUnitProperty_ElementCount:
        size = sizeof(UInt32); writable = false; return noErr;
    case kAudioUnitProperty_ParameterList:
        size = 0; return noErr;
    case kAudioUnitProperty_Latency:
    case kAudioUnitProperty_TailTime:
        if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        size = sizeof(Float64); return noErr;
    case kAudioUnitProperty_SupportedNumChannels:
        if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        size = sizeof(AUChannelInfo); return noErr;
    case kAudioUnitProperty_MaximumFramesPerSlice:
        if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        size = sizeof(UInt32); writable = true; return noErr;
    case kAudioUnitProperty_LastRenderError:
        if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        size = sizeof(OSStatus); return noErr;
    case kAudioUnitProperty_HostCallbacks:
        if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        size = sizeof(HostCallbackInfo); writable = true; return noErr;
    case kAudioUnitProperty_CocoaUI:
        if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        size = sizeof(AudioUnitCocoaViewInfo); return noErr;
    case kAudioUnitProperty_PresentPreset:
        if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        size = sizeof(AUPreset); writable = true; return noErr;
    case kAudioUnitProperty_OfflineRender:
        if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        size = sizeof(UInt32); writable = true; return noErr;
    case kAudioUnitProperty_ShouldAllocateBuffer:
        if (scope != kAudioUnitScope_Output) return kAudioUnitErr_InvalidScope;
        size = sizeof(UInt32); writable = true; return noErr;
    case kAudioUnitProperty_InPlaceProcessing:
        size = sizeof(UInt32); writable = true; return noErr;
    case ob12mac::kPropertyMachine:
        if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        size = sizeof(void*); return noErr;
    }
    return kAudioUnitErr_InvalidProperty;
}

OSStatus get_property(Plugin* p, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element, void* data, UInt32& size) {
    UInt32 need = 0;
    Boolean writable = false;
    OSStatus status = property_info(p, id, scope, element, need, writable);
    if (status) return status;
    if (size < need) return kAudioUnitErr_InvalidPropertyValue;
    size = need;
    switch (id) {
    case kAudioUnitProperty_ClassInfo: {
        NSMutableDictionary* d = [NSMutableDictionary dictionary];
        d[@kAUPresetVersionKey] = @0;
        d[@kAUPresetTypeKey] = @((SInt32)p->description.componentType);
        d[@kAUPresetSubtypeKey] = @((SInt32)p->description.componentSubType);
        d[@kAUPresetManufacturerKey] = @((SInt32)p->description.componentManufacturer);
        d[@kAUPresetNameKey] = [NSString stringWithUTF8String:p->preset_name.c_str()] ?: @"Untitled";
        d[@kAUPresetDataKey] = [NSData data];  // the unit's memory is its flash, kept per user
        *(CFPropertyListRef*)data = CFBridgingRetain(d);
        return noErr;
    }
    case kAudioUnitProperty_StreamFormat: *(AudioStreamBasicDescription*)data = p->format(); return noErr;
    case kAudioUnitProperty_SampleRate: *(Float64*)data = p->rate; return noErr;
    case kAudioUnitProperty_ElementCount:
        *(UInt32*)data = scope == kAudioUnitScope_Input ? 0 : 1;
        return noErr;
    case kAudioUnitProperty_ParameterList: return noErr;
    // The ring keeps lead_frames of the engine's 48 kHz ahead: the host can line the instrument up.
    case kAudioUnitProperty_Latency: *(Float64*)data = lead_frames / 48000.0; return noErr;
    case kAudioUnitProperty_TailTime: *(Float64*)data = 0; return noErr;
    case kAudioUnitProperty_SupportedNumChannels: {
        AUChannelInfo info = {0, 2};
        *(AUChannelInfo*)data = info;
        return noErr;
    }
    case kAudioUnitProperty_MaximumFramesPerSlice: *(UInt32*)data = p->max_frames; return noErr;
    case kAudioUnitProperty_LastRenderError: *(OSStatus*)data = p->last_error; p->last_error = noErr; return noErr;
    case kAudioUnitProperty_HostCallbacks: *(HostCallbackInfo*)data = p->host; return noErr;
    case kAudioUnitProperty_CocoaUI: {
        AudioUnitCocoaViewInfo* info = (AudioUnitCocoaViewInfo*)data;
        NSString* path = [NSString stringWithUTF8String:bundle_path().c_str()];
        info->mCocoaAUViewBundleLocation = (CFURLRef)CFBridgingRetain([NSURL fileURLWithPath:path]);
        info->mCocoaAUViewClass[0] = CFSTR(OB12_STR(OB12_VIEW_FACTORY));
        CFRetain(info->mCocoaAUViewClass[0]);
        return noErr;
    }
    case kAudioUnitProperty_PresentPreset: {
        AUPreset* preset = (AUPreset*)data;
        preset->presetNumber = p->preset_number;
        preset->presetName = CFStringCreateWithCString(nullptr, p->preset_name.c_str(), kCFStringEncodingUTF8);
        return noErr;
    }
    case kAudioUnitProperty_OfflineRender: *(UInt32*)data = p->offline; return noErr;
    case kAudioUnitProperty_ShouldAllocateBuffer: *(UInt32*)data = p->allocate; return noErr;
    case kAudioUnitProperty_InPlaceProcessing: *(UInt32*)data = 0; return noErr;  // no input to process in place
    case ob12mac::kPropertyMachine: *(void**)data = p; return noErr;
    }
    return kAudioUnitErr_InvalidProperty;
}

OSStatus set_property(Plugin* p, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element, const void* data, UInt32 size) {
    UInt32 need = 0;
    Boolean writable = false;
    OSStatus status = property_info(p, id, scope, element, need, writable);
    if (status) return status;
    if (!writable) return kAudioUnitErr_PropertyNotWritable;
    if (!data) return kAudioUnitErr_InvalidPropertyValue;
    if (id != kAudioUnitProperty_ClassInfo && size < need) return kAudioUnitErr_InvalidPropertyValue;
    switch (id) {
    case kAudioUnitProperty_ClassInfo: {
        if (size < sizeof(CFPropertyListRef)) return kAudioUnitErr_InvalidPropertyValue;
        CFPropertyListRef list = *(const CFPropertyListRef*)data;
        if (!list || CFGetTypeID(list) != CFDictionaryGetTypeID()) return kAudioUnitErr_InvalidPropertyValue;
        NSDictionary* d = (__bridge NSDictionary*)list;
        NSNumber* type = d[@kAUPresetTypeKey];
        NSNumber* subtype = d[@kAUPresetSubtypeKey];
        NSNumber* maker = d[@kAUPresetManufacturerKey];
        if (![type isKindOfClass:[NSNumber class]] || ![subtype isKindOfClass:[NSNumber class]] || ![maker isKindOfClass:[NSNumber class]])
            return kAudioUnitErr_InvalidPropertyValue;
        if ((OSType)type.intValue != p->description.componentType || (OSType)subtype.intValue != p->description.componentSubType ||
            (OSType)maker.intValue != p->description.componentManufacturer)
            return kAudioUnitErr_InvalidPropertyValue;
        NSString* name = d[@kAUPresetNameKey];
        if ([name isKindOfClass:[NSString class]]) p->preset_name = name.UTF8String;
        p->preset_number = -1;
        p->changed(kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0);
        return noErr;
    }
    case kAudioUnitProperty_StreamFormat: {
        const AudioStreamBasicDescription& f = *(const AudioStreamBasicDescription*)data;
        bool ok = f.mFormatID == kAudioFormatLinearPCM && (f.mFormatFlags & kAudioFormatFlagIsFloat) &&
                  (f.mFormatFlags & kAudioFormatFlagIsNonInterleaved) && !(f.mFormatFlags & kAudioFormatFlagIsBigEndian) &&
                  f.mBitsPerChannel == 32 && f.mBytesPerFrame == 4 && f.mChannelsPerFrame == 2 && f.mSampleRate >= 8000 && f.mSampleRate <= 768000;
        if (!ok) return kAudioUnitErr_FormatNotSupported;
        bool rate_changed = f.mSampleRate != p->rate;
        p->rate = f.mSampleRate;
        p->changed(kAudioUnitProperty_StreamFormat, scope, element);
        if (rate_changed) p->changed(kAudioUnitProperty_SampleRate, scope, element);
        return noErr;
    }
    case kAudioUnitProperty_SampleRate: {
        Float64 r = *(const Float64*)data;
        if (r < 8000 || r > 768000) return kAudioUnitErr_InvalidPropertyValue;
        p->rate = r;
        p->changed(kAudioUnitProperty_SampleRate, scope, element);
        p->changed(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0);
        return noErr;
    }
    case kAudioUnitProperty_MaximumFramesPerSlice: {
        UInt32 n = *(const UInt32*)data;
        if (n == 0) return kAudioUnitErr_InvalidPropertyValue;
        // Not while initialized: au_render may be using the buffers sized for the old value.
        if (p->initialized) return n == p->max_frames ? noErr : kAudioUnitErr_Initialized;
        p->max_frames = n;
        p->changed(kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0);
        return noErr;
    }
    case kAudioUnitProperty_HostCallbacks: {
        HostCallbackInfo info = {};
        memcpy(&info, data, std::min<UInt32>(size, sizeof info));
        p->host = info;
        return noErr;
    }
    case kAudioUnitProperty_PresentPreset: {
        const AUPreset* preset = (const AUPreset*)data;
        p->preset_number = preset->presetNumber;
        if (preset->presetName) {
            char text[256] = {};
            if (CFStringGetCString(preset->presetName, text, sizeof text, kCFStringEncodingUTF8)) p->preset_name = text;
        }
        return noErr;
    }
    case kAudioUnitProperty_OfflineRender: p->offline = *(const UInt32*)data; return noErr;
    case kAudioUnitProperty_ShouldAllocateBuffer: p->allocate = *(const UInt32*)data; return noErr;
    case kAudioUnitProperty_InPlaceProcessing: return noErr;
    }
    return kAudioUnitErr_InvalidProperty;
}

// ---------------------------------------------------------------- the selectors

OSStatus au_initialize(void* self) {
    Plugin* p = me(self);
    if (p->initialized) return noErr;
    for (auto& b : p->own) b.assign(p->max_frames, 0.f);
    p->ensure();  // boots the machine; a missing firmware leaves it silent, not failed
    p->initialized = true;
    return noErr;
}

OSStatus au_uninitialize(void* self) {
    // The machine keeps running: booting takes seconds and its RAM is the unit's edit buffer.
    me(self)->initialized = false;
    return noErr;
}

OSStatus au_get_property_info(void* self, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element, UInt32* size, Boolean* writable) {
    UInt32 s = 0;
    Boolean w = false;
    OSStatus status = property_info(me(self), id, scope, element, s, w);
    if (status) return status;
    if (size) *size = s;
    if (writable) *writable = w;
    return noErr;
}

OSStatus au_get_property(void* self, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element, void* data, UInt32* size) {
    if (!size) return kAudio_ParamError;
    if (!data) {  // the size only
        Boolean w;
        return property_info(me(self), id, scope, element, *size, w);
    }
    @autoreleasepool {
        return get_property(me(self), id, scope, element, data, *size);
    }
}

OSStatus au_set_property(void* self, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element, const void* data, UInt32 size) {
    @autoreleasepool {
        return set_property(me(self), id, scope, element, data, size);
    }
}

OSStatus au_add_listener(void* self, AudioUnitPropertyID id, AudioUnitPropertyListenerProc proc, void* data) {
    std::lock_guard<std::mutex> hold(me(self)->listener_lock);
    me(self)->listeners.push_back({id, proc, data});
    return noErr;
}

OSStatus au_remove_listener(void* self, AudioUnitPropertyID id, AudioUnitPropertyListenerProc proc) {
    std::lock_guard<std::mutex> hold(me(self)->listener_lock);
    auto& l = me(self)->listeners;
    l.erase(std::remove_if(l.begin(), l.end(), [&](const Listener& x) { return x.id == id && x.proc == proc; }), l.end());
    return noErr;
}

OSStatus au_remove_listener_data(void* self, AudioUnitPropertyID id, AudioUnitPropertyListenerProc proc, void* data) {
    std::lock_guard<std::mutex> hold(me(self)->listener_lock);
    auto& l = me(self)->listeners;
    l.erase(std::remove_if(l.begin(), l.end(), [&](const Listener& x) { return x.id == id && x.proc == proc && x.data == data; }), l.end());
    return noErr;
}

OSStatus au_get_parameter(void*, AudioUnitParameterID, AudioUnitScope, AudioUnitElement, AudioUnitParameterValue*) {
    return kAudioUnitErr_InvalidParameter;
}
OSStatus au_set_parameter(void*, AudioUnitParameterID, AudioUnitScope, AudioUnitElement, AudioUnitParameterValue, UInt32) {
    return kAudioUnitErr_InvalidParameter;
}
OSStatus au_schedule_parameters(void*, const AudioUnitParameterEvent*, UInt32 count) {
    return count ? kAudioUnitErr_InvalidParameter : noErr;
}

OSStatus au_reset(void*, AudioUnitScope, AudioUnitElement) { return noErr; }

OSStatus au_add_notify(void* self, AURenderCallback proc, void* data) {
    std::lock_guard<std::mutex> hold(me(self)->notify_lock);
    me(self)->notifies.push_back({proc, data});
    return noErr;
}
OSStatus au_remove_notify(void* self, AURenderCallback proc, void* data) {
    std::lock_guard<std::mutex> hold(me(self)->notify_lock);
    auto& n = me(self)->notifies;
    n.erase(std::remove_if(n.begin(), n.end(), [&](const Notify& x) { return x.proc == proc && x.data == data; }), n.end());
    return noErr;
}

OSStatus au_render(void* self, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* time, UInt32 bus, UInt32 frames, AudioBufferList* io) {
    Plugin* p = me(self);
    if (!p->initialized) return kAudioUnitErr_Uninitialized;
    if (bus != 0) return kAudioUnitErr_InvalidElement;
    if (frames > p->max_frames) { p->last_error = kAudioUnitErr_TooManyFramesToProcess; return kAudioUnitErr_TooManyFramesToProcess; }
    if (!io || io->mNumberBuffers < 1) return kAudio_ParamError;

    AudioUnitRenderActionFlags pre = (flags ? *flags : 0) | kAudioUnitRenderAction_PreRender;
    Notify list[8];
    int listed = 0;
    if (p->notify_lock.try_lock()) {
        for (auto& n : p->notifies) if (listed < 8) list[listed++] = n;
        p->notify_lock.unlock();
    }
    for (int i = 0; i < listed; i++) list[i].proc(list[i].data, &pre, time, bus, frames, io);

    for (UInt32 b = 0; b < io->mNumberBuffers && b < 2; b++) {
        if (!io->mBuffers[b].mData) io->mBuffers[b].mData = p->own[b].data();
        io->mBuffers[b].mDataByteSize = frames * sizeof(float);
    }
    float* l = (float*)io->mBuffers[0].mData;
    float* r = io->mNumberBuffers > 1 ? (float*)io->mBuffers[1].mData : nullptr;

    Instrument* inst = p->live.load(std::memory_order_acquire);
    if (!inst || !inst->engine) {
        memset(l, 0, frames * sizeof(float));
        if (r) memset(r, 0, frames * sizeof(float));
        if (flags) *flags |= kAudioUnitRenderAction_OutputIsSilence;
    } else {
        inst->step = 48000.0 / p->rate;
        inst->render(l, r, (int)frames, p->offline != 0);
    }

    AudioUnitRenderActionFlags post = (flags ? *flags : 0) | kAudioUnitRenderAction_PostRender;
    for (int i = 0; i < listed; i++) list[i].proc(list[i].data, &post, time, bus, frames, io);
    return noErr;
}

// MIDI: each message keeps its channel, as on the unit's MIDI IN; the Master decides what it hears.
OSStatus au_midi_event(void* self, UInt32 status, UInt32 data1, UInt32 data2, UInt32) {
    if (status < 0x80 || status > 0xff) return noErr;
    Instrument* inst = me(self)->live.load(std::memory_order_acquire);
    if (!inst || !inst->engine) return noErr;
    uint8_t bytes[3] = {(uint8_t)status, (uint8_t)(data1 & 0x7f), (uint8_t)(data2 & 0x7f)};
    int count = 3;
    if (status >= 0xf8 || status == 0xf6) count = 1;
    else if (status == 0xf1 || status == 0xf3 || (status >= 0xc0 && status < 0xe0)) count = 2;
    inst->engine->midi(bytes, count);
    return noErr;
}

OSStatus au_sysex(void* self, const UInt8* data, UInt32 length) {
    Instrument* inst = me(self)->live.load(std::memory_order_acquire);
    if (!inst || !inst->engine || !data || !length) return noErr;
    inst->engine->midi(data, (int)std::min<UInt32>(length, 1 << 20));
    return noErr;
}

OSStatus au_start_note(void*, MusicDeviceInstrumentID, MusicDeviceGroupID, NoteInstanceID*, UInt32, const MusicDeviceNoteParams*) {
    return kAudio_UnimplementedError;
}
OSStatus au_stop_note(void*, MusicDeviceGroupID, NoteInstanceID, UInt32) { return kAudio_UnimplementedError; }

AudioComponentMethod lookup(SInt16 selector) {
    switch (selector) {
    case kAudioUnitInitializeSelect: return (AudioComponentMethod)au_initialize;
    case kAudioUnitUninitializeSelect: return (AudioComponentMethod)au_uninitialize;
    case kAudioUnitGetPropertyInfoSelect: return (AudioComponentMethod)au_get_property_info;
    case kAudioUnitGetPropertySelect: return (AudioComponentMethod)au_get_property;
    case kAudioUnitSetPropertySelect: return (AudioComponentMethod)au_set_property;
    case kAudioUnitAddPropertyListenerSelect: return (AudioComponentMethod)au_add_listener;
    case kAudioUnitRemovePropertyListenerSelect: return (AudioComponentMethod)au_remove_listener;
    case kAudioUnitRemovePropertyListenerWithUserDataSelect: return (AudioComponentMethod)au_remove_listener_data;
    case kAudioUnitGetParameterSelect: return (AudioComponentMethod)au_get_parameter;
    case kAudioUnitSetParameterSelect: return (AudioComponentMethod)au_set_parameter;
    case kAudioUnitScheduleParametersSelect: return (AudioComponentMethod)au_schedule_parameters;
    case kAudioUnitRenderSelect: return (AudioComponentMethod)au_render;
    case kAudioUnitResetSelect: return (AudioComponentMethod)au_reset;
    case kAudioUnitAddRenderNotifySelect: return (AudioComponentMethod)au_add_notify;
    case kAudioUnitRemoveRenderNotifySelect: return (AudioComponentMethod)au_remove_notify;
    case kMusicDeviceMIDIEventSelect: return (AudioComponentMethod)au_midi_event;
    case kMusicDeviceSysExSelect: return (AudioComponentMethod)au_sysex;
    case kMusicDeviceStartNoteSelect: return (AudioComponentMethod)au_start_note;
    case kMusicDeviceStopNoteSelect: return (AudioComponentMethod)au_stop_note;
    }
    return nullptr;
}

OSStatus au_open(void* self, AudioComponentInstance instance) {
    Plugin* p = me(self);
    p->instance = instance;
    AudioComponentGetDescription(AudioComponentInstanceGetComponent(instance), &p->description);
    return noErr;
}

OSStatus au_close(void* self) {
    delete me(self);  // the view, if still open, keeps the machine through its own reference
    return noErr;
}

}  // namespace

namespace ob12mac {

std::string support_folder(bool make) {
    const char* home = getenv("HOME");
    std::string folder = std::string(home ? home : "/tmp") + "/Library/Application Support/OB12";
    if (make) mkdir(folder.c_str(), 0755);
    return folder;
}

std::string rom_folder() {
    if (const char* env = getenv("OB12_ROM")) return env;
    std::string inside = bundle_path() + "/Contents/Resources/rom";
    if (exists(inside + "/12cpu152-CPU-v1.52-FINAL.bin")) return inside;
    return support_folder(false) + "/rom";
}

bool acquire(void* plugin, MachineRef& out) {
    if (!plugin) return false;
    std::shared_ptr<Instrument> inst = ((Plugin*)plugin)->ensure();
    if (!inst || !inst->engine) return false;
    out.engine = inst->engine.get();
    out.keep = inst;
    return true;
}

}  // namespace ob12mac

extern "C" __attribute__((visibility("default"))) void* OB12AUFactory(const AudioComponentDescription*) {
    Plugin* p = new Plugin;
    p->face.Open = au_open;
    p->face.Close = au_close;
    p->face.Lookup = lookup;
    p->face.reserved = nullptr;
    return p;
}
