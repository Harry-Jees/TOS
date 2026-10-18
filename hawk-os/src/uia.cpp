// Hawk OS - semantic Windows accessibility perception.
// UI Automation supplies names, roles, bounds, and enabled/off-screen state.
// Pixel-based perception remains the fallback for inaccessible/custom-drawn apps.
#include "hawk.h"

#include <UIAutomation.h>
#include <oleauto.h>

#include <functional>
#include <sstream>

namespace hawk {

static const char* control_type_name(CONTROLTYPEID type) {
    switch (type) {
        case UIA_ButtonControlTypeId: return "button";
        case UIA_EditControlTypeId: return "edit";
        case UIA_TextControlTypeId: return "text";
        case UIA_WindowControlTypeId: return "window";
        case UIA_CheckBoxControlTypeId: return "checkbox";
        case UIA_ComboBoxControlTypeId: return "combobox";
        case UIA_ListItemControlTypeId: return "list-item";
        case UIA_MenuItemControlTypeId: return "menu-item";
        case UIA_TabItemControlTypeId: return "tab";
        case UIA_HyperlinkControlTypeId: return "link";
        case UIA_RadioButtonControlTypeId: return "radio";
        case UIA_SliderControlTypeId: return "slider";
        case UIA_PaneControlTypeId: return "pane";
        case UIA_ToolBarControlTypeId: return "toolbar";
        case UIA_GroupControlTypeId: return "group";
        default: return "control";
    }
}

std::string describe_accessible_controls(HWND hwnd, int max_elements) {
    if (!hwnd || max_elements <= 0) return "";
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool uninitialize = SUCCEEDED(init);
    IUIAutomation* automation = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&automation));
    if (FAILED(hr) || !automation) {
        if (uninitialize) CoUninitialize();
        return "";
    }

    IUIAutomationElement* root = nullptr;
    IUIAutomationTreeWalker* walker = nullptr;
    hr = automation->ElementFromHandle(hwnd, &root);
    if (SUCCEEDED(hr) && root) hr = automation->get_ControlViewWalker(&walker);
    if (FAILED(hr) || !root || !walker) {
        if (walker) walker->Release();
        if (root) root->Release();
        automation->Release();
        if (uninitialize) CoUninitialize();
        return "";
    }

    std::ostringstream out;
    int visited = 0, reported = 0;
    std::function<void(IUIAutomationElement*, int)> walk;
    walk = [&](IUIAutomationElement* parent, int depth) {
        if (!parent || depth > 6 || visited >= max_elements * 6 || reported >= max_elements) return;
        IUIAutomationElement* child = nullptr;
        if (FAILED(walker->GetFirstChildElement(parent, &child)) || !child) return;
        while (child && visited < max_elements * 6 && reported < max_elements) {
            ++visited;
            BSTR raw_name = nullptr;
            CONTROLTYPEID type = 0;
            RECT rect{};
            BOOL enabled = TRUE, offscreen = FALSE;
            child->get_CurrentName(&raw_name);
            child->get_CurrentControlType(&type);
            child->get_CurrentBoundingRectangle(&rect);
            child->get_CurrentIsEnabled(&enabled);
            child->get_CurrentIsOffscreen(&offscreen);

            std::wstring wide_name;
            if (raw_name) {
                wide_name.assign(raw_name, SysStringLen(raw_name));
                SysFreeString(raw_name);
            }
            std::string name = narrow(wide_name);
            if (!offscreen && rect.right > rect.left && rect.bottom > rect.top &&
                (!name.empty() || type == UIA_ButtonControlTypeId ||
                 type == UIA_EditControlTypeId || type == UIA_CheckBoxControlTypeId ||
                 type == UIA_ComboBoxControlTypeId || type == UIA_HyperlinkControlTypeId)) {
                out << control_type_name(type) << " name=\"" << name << "\""
                    << " rect=" << rect.left << "," << rect.top << ","
                    << (rect.right - rect.left) << "x" << (rect.bottom - rect.top)
                    << " enabled=" << (enabled ? "true" : "false") << "\n";
                ++reported;
            }
            if (depth < 6 && visited < max_elements * 6 && reported < max_elements)
                walk(child, depth + 1);

            IUIAutomationElement* next = nullptr;
            HRESULT next_hr = walker->GetNextSiblingElement(child, &next);
            child->Release();
            child = SUCCEEDED(next_hr) ? next : nullptr;
        }
    };
    walk(root, 0);

    walker->Release();
    root->Release();
    automation->Release();
    if (uninitialize) CoUninitialize();
    return out.str();
}

}  // namespace hawk
