// Loads the bundle as a host would and plays one note through process().
// usage: vst3_smoke <path to OB12.vst3 dll>
#define NOMINMAX
#include <windows.h>
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/gui/iplugview.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace Steinberg;
using namespace Steinberg::Vst;

struct Events final : IEventList {
    std::vector<Event> list;
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    int32 PLUGIN_API getEventCount() override { return (int32)list.size(); }
    tresult PLUGIN_API getEvent(int32 i, Event& e) override { e = list[i]; return kResultOk; }
    tresult PLUGIN_API addEvent(Event& e) override { list.push_back(e); return kResultOk; }
};

// A host frame; calls back onSize when it is that kind of host.
struct Frame final : IPlugFrame {
    bool calls_back = true;
    int requests = 0;
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* r) override {
        requests++;
        if (calls_back) view->onSize(r);
        return kResultOk;
    }
};

static void test_scale(IComponent* component) {
    IEditController* controller = nullptr;
    component->queryInterface(IEditController::iid, (void**)&controller);
    IPlugView* view = controller->createView(ViewType::kEditor);
    ViewRect size;
    view->getSize(&size);
    printf("editor opens at %d x %d\n", size.getWidth(), size.getHeight());
    HWND parent = CreateWindowW(L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, size.getWidth(), size.getHeight(), nullptr, nullptr, nullptr, nullptr);
    Frame frame;
    view->setFrame(&frame);
    view->attached(parent, kPlatformTypeHWND);
    HWND child = FindWindowExW(parent, nullptr, nullptr, nullptr);
    for (int percent : {50, 67, 200, 100}) {
        frame.calls_back = percent != 67;
        SendMessageW(child, WM_COMMAND, 1000 + percent, 0);
        RECT rc;
        GetClientRect(child, &rc);
        view->getSize(&size);
        printf("%d%%%s: view %d x %d, window %ld x %ld\n", percent, frame.calls_back ? "" : " (host silent)", size.getWidth(), size.getHeight(), rc.right, rc.bottom);
    }
    SendMessageW(child, WM_COMMAND, 1000 + 75, 0);
    view->removed();
    view->release();
    IPlugView* again = controller->createView(ViewType::kEditor);
    again->getSize(&size);
    printf("reopens at %d x %d, %d requests\n", size.getWidth(), size.getHeight(), frame.requests);
    again->release();
    DestroyWindow(parent);
    controller->release();
}

int main(int, char** argv) {
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    SetEnvironmentVariableA("LOCALAPPDATA", temp);  // keep the user's saved size out of this
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) { puts("load failed"); return 1; }
    auto get = (IPluginFactory * (PLUGIN_API*)())GetProcAddress(dll, "GetPluginFactory");
    IPluginFactory* factory = get();
    PClassInfo info;
    factory->getClassInfo(0, &info);
    printf("class %s\n", info.name);
    IComponent* component = nullptr;
    factory->createInstance(info.cid, IComponent::iid, (void**)&component);
    component->initialize(nullptr);
    IAudioProcessor* audio = nullptr;
    component->queryInterface(IAudioProcessor::iid, (void**)&audio);
    ProcessSetup setup{kRealtime, kSample32, 512, 44100};  // not the machine's rate, on purpose
    audio->setupProcessing(setup);
    component->setActive(true);
    audio->setProcessing(true);

    std::vector<float> l(512), r(512);
    float* channels[2] = {l.data(), r.data()};
    AudioBusBuffers bus{};
    bus.numChannels = 2;
    bus.channelBuffers32 = channels;
    Events events;
    ProcessData data{};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = 512;
    data.numOutputs = 1;
    data.outputs = &bus;
    data.inputEvents = &events;

    // Blocks are paced by the wall clock, as a sound card would.
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    auto seconds = [&]() { QueryPerformanceCounter(&now); return (double)(now.QuadPart - start.QuadPart) / freq.QuadPart; };
    double block = 512 / 44100.0, next = 0, sum = 0, first_sound = -1;
    long frames = 0, zero_blocks = 0, blocks = 0;
    bool sent = false;
    while (next < 17) {
        while (seconds() < next) Sleep(1);
        next += block;
        events.list.clear();
        if (!sent && next >= 11) {
            Event e{};
            e.type = Event::kNoteOnEvent;
            e.noteOn.pitch = 72;
            e.noteOn.velocity = 0.8f;
            events.list.push_back(e);
            sent = true;
        }
        audio->process(data);
        double s = 0;
        for (float v : l) s += v * v;
        if (s > 0 && first_sound < 0) first_sound = next;
        { static int nz = 0, n = 0; n++; if (s > 0) nz++; if (n == 43) { printf("%.1f:%d ", next, nz); n = nz = 0; } }
        if (next > 13) {
            sum += s;
            frames += 512;
            blocks++;
            if (s == 0) zero_blocks++;
        }
    }
    printf("first sound at %.2f s; rms %.4f over %ld frames, %ld of %ld blocks empty\n", first_sound, sqrt(sum / frames), frames, zero_blocks, blocks);
    test_scale(component);
    audio->setProcessing(false);
    component->setActive(false);
    component->terminate();
    component->release();
    return 0;
}
