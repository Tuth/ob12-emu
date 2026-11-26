// Shared between the Audio Unit (ob12_au.mm) and its Cocoa panel (panel_view_mac.mm).
#pragma once
#include <memory>
#include <string>

class Engine;

namespace ob12mac {

// ~/Library/Application Support/OB12: the unit's flash, the output gain, the editor width.
std::string support_folder(bool make);

// Where the firmware is: $OB12_ROM, else the bundle's Contents/Resources/rom, else
// ~/Library/Application Support/OB12/rom.
std::string rom_folder();

// The plug-in's machine as the panel sees it. keep holds the machine alive while the view lives,
// even if the host disposes of the plug-in first.
struct MachineRef {
    std::shared_ptr<void> keep;
    Engine* engine = nullptr;
};

// plugin: the pointer the AU returns for kPropertyMachine. Boots the machine if it is not running.
bool acquire(void* plugin, MachineRef& out);

// Private AU property, global scope: a void*, the plug-in object, for the view in the same image.
enum { kPropertyMachine = 64012 };

}  // namespace ob12mac
