// Hawk OS - semantic Windows accessibility perception and control.
// Prefer UI Automation control patterns over guessed screen coordinates. Pixel
// perception and physical input remain fallbacks for custom-drawn/inaccessible apps.
#include "hawk.h"

#include <UIAutomation.h>
#include <oleauto.h>

#include <algorithm>
#include <cwctype>
#include <functional>
#include <sstream>
#include <vector>

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
        case UIA_ListControlTypeId: return "list";
        case UIA_DataGridControlTypeId: return "data-grid";
        case UIA_TableControlTypeId: return "table";
        case UIA_MenuControlTypeId: return "menu";
        case UIA_TreeControlTypeId: return "tree";
        case UIA_TreeItemControlTypeId: return "tree-item";
        case UIA_SplitButtonControlTypeId: return "split-button";
        case UIA_SpinnerControlTypeId: return "spinner";
        default: return "control";
    }
}

static std::wstring lower_wide(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return (wchar_t)towlower(c); });
    return value;
}

static std::wstring element_name(IUIAutomationElement* element) {
    BSTR name = nullptr;
    std::wstring result;
    if (element && SUCCEEDED(element->get_CurrentName(&name)) && name) {
        result.assign(name, SysStringLen(name));
        SysFreeString(name);
    }
    return result;
}

static std::wstring element_value(IUIAutomationElement* element) {
    if (!element) return L"";
    IUnknown* unknown = nullptr;
    if (FAILED(element->GetCurrentPattern(UIA_ValuePatternId, &unknown)) || !unknown)
        return L"";
    IUIAutomationValuePattern* pattern = nullptr;
    HRESULT hr = unknown->QueryInterface(IID_PPV_ARGS(&pattern));
    unknown->Release();
    if (FAILED(hr) || !pattern) return L"";
    BSTR value = nullptr;
    std::wstring result;
    if (SUCCEEDED(pattern->get_CurrentValue(&value)) && value) {
        result.assign(value, SysStringLen(value));
        SysFreeString(value);
    }
    pattern->Release();
    return result;
}

static void one_line(std::string* value) {
    if (!value) return;
    for (char& ch : *value)
        if (ch == '\r' || ch == '\n' || ch == '\t' || ch == '"' || ch == '\\')
            ch = ' ';
}

// Get an accessibility tree from a single app window. Bound traversal to avoid
// hanging on pathological providers or enumerating the entire desktop.
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
        if (!parent || depth > 8 || visited >= max_elements * 10 || reported >= max_elements) return;
        IUIAutomationElement* child = nullptr;
        if (FAILED(walker->GetFirstChildElement(parent, &child)) || !child) return;
        while (child && visited < max_elements * 10 && reported < max_elements) {
            ++visited;
            CONTROLTYPEID type = 0;
            RECT rect{};
            BOOL enabled = TRUE, offscreen = FALSE;
            child->get_CurrentControlType(&type);
            child->get_CurrentBoundingRectangle(&rect);
            child->get_CurrentIsEnabled(&enabled);
            child->get_CurrentIsOffscreen(&offscreen);

            std::string name = narrow(element_name(child));
            one_line(&name);
            if (!offscreen && rect.right > rect.left && rect.bottom > rect.top &&
                (!name.empty() || type == UIA_ButtonControlTypeId ||
                 type == UIA_EditControlTypeId || type == UIA_CheckBoxControlTypeId ||
                 type == UIA_ComboBoxControlTypeId || type == UIA_HyperlinkControlTypeId ||
                 type == UIA_MenuItemControlTypeId)) {
                out << control_type_name(type) << " name=\"" << name << "\""
                    << " rect=" << rect.left << "," << rect.top << ","
                    << (rect.right - rect.left) << "x" << (rect.bottom - rect.top)
                    << " enabled=" << (enabled ? "true" : "false") << "\n";
                ++reported;
            }
            if (depth < 8 && visited < max_elements * 10 && reported < max_elements)
                walk(child, depth + 1);

            IUIAutomationElement* next = nullptr;
            HRESULT next_hr = walker->GetNextSiblingElement(child, &next);
            child->Release();
            child = SUCCEEDED(next_hr) ? next : nullptr;
        }
        if (child) child->Release(); // release a sibling left by an early traversal stop
    };
    walk(root, 0);

    walker->Release();
    root->Release();
    automation->Release();
    if (uninitialize) CoUninitialize();
    return out.str();
}

// Resolve a control by its accessible Name in the foreground window. Exact
// matches win; substring matching is allowed only when unique. Ambiguous matches
// fail rather than risking an action on the wrong control.
bool act_accessible_control(HWND hwnd, const std::string& action,
                            const std::string& requested_name,
                            const std::string& requested_value,
                            std::string* result) {
    if (result) result->clear();
    if (!hwnd) hwnd = GetForegroundWindow();
    if (!hwnd || !IsWindow(hwnd)) {
        if (result) *result = "no valid target window";
        return false;
    }
    const std::wstring needle = lower_wide(wide(trim(requested_name)));
    if (needle.empty()) {
        if (result) *result = "control name is empty";
        return false;
    }

    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool uninitialize = SUCCEEDED(init);
    IUIAutomation* automation = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&automation));
    if (FAILED(hr) || !automation) {
        if (uninitialize) CoUninitialize();
        if (result) *result = "Windows UI Automation could not be initialized";
        return false;
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
        if (result) *result = "could not read the target window's accessibility tree";
        return false;
    }

    IUIAutomationElement* exact = nullptr;
    IUIAutomationElement* partial = nullptr;
    int partial_count = 0;
    int visited = 0;
    std::function<void(IUIAutomationElement*, int)> walk;
    walk = [&](IUIAutomationElement* parent, int depth) {
        if (!parent || exact || depth > 12 || visited >= 2500) return;
        IUIAutomationElement* child = nullptr;
        if (FAILED(walker->GetFirstChildElement(parent, &child)) || !child) return;
        while (child && !exact && visited < 2500) {
            ++visited;
            BOOL enabled = TRUE, offscreen = FALSE;
            child->get_CurrentIsEnabled(&enabled);
            child->get_CurrentIsOffscreen(&offscreen);
            const std::wstring name = lower_wide(element_name(child));
            if (enabled && !offscreen && !name.empty()) {
                if (name == needle) {
                    child->AddRef();
                    exact = child;
                } else if (name.find(needle) != std::wstring::npos) {
                    ++partial_count;
                    if (!partial) {
                        child->AddRef();
                        partial = child;
                    }
                }
            }
            if (!exact && depth < 12 && visited < 2500) walk(child, depth + 1);
            IUIAutomationElement* next = nullptr;
            HRESULT next_hr = walker->GetNextSiblingElement(child, &next);
            child->Release();
            child = SUCCEEDED(next_hr) ? next : nullptr;
        }
        if (child) child->Release(); // release a sibling left by an early traversal stop
    };
    walk(root, 0);

    IUIAutomationElement* target = exact;
    if (!target && partial_count == 1) {
        target = partial;
        partial = nullptr;
    }
    // Keep the selected element's AddRef alive until the action has completed.
    if (exact && target != exact) exact->Release();
    if (partial) partial->Release();
    walker->Release();
    root->Release();
    automation->Release();

    if (!target) {
        if (uninitialize) CoUninitialize();
        if (result) {
            if (partial_count > 1)
                *result = "control name is ambiguous; use a more specific accessible name";
            else
                *result = "no enabled, visible control matched the requested name";
        }
        return false;
    }

    const std::string op = to_lower(trim(action));
    bool ok = false;
    HRESULT action_hr = E_NOINTERFACE;
    if (op == "read") {
        std::wstring value = element_value(target);
        if (value.empty()) value = element_name(target);
        if (result) *result = narrow(value);
        ok = true;
    } else if (op == "focus") {
        action_hr = target->SetFocus();
        ok = SUCCEEDED(action_hr);
    } else if (op == "set") {
        IUnknown* unknown = nullptr;
        action_hr = target->GetCurrentPattern(UIA_ValuePatternId, &unknown);
        if (SUCCEEDED(action_hr) && unknown) {
            IUIAutomationValuePattern* pattern = nullptr;
            action_hr = unknown->QueryInterface(IID_PPV_ARGS(&pattern));
            unknown->Release();
            if (SUCCEEDED(action_hr) && pattern) {
                action_hr = pattern->SetValue(wide(requested_value).c_str());
                pattern->Release();
            }
        }
        ok = SUCCEEDED(action_hr);
    } else {
        int pattern_id = 0;
        if (op == "invoke" || op == "click") pattern_id = UIA_InvokePatternId;
        else if (op == "toggle") pattern_id = UIA_TogglePatternId;
        else if (op == "select") pattern_id = UIA_SelectionItemPatternId;
        else if (op == "expand" || op == "collapse") pattern_id = UIA_ExpandCollapsePatternId;
        if (pattern_id) {
            IUnknown* unknown = nullptr;
            action_hr = target->GetCurrentPattern(pattern_id, &unknown);
            if (SUCCEEDED(action_hr) && unknown) {
                if (pattern_id == UIA_InvokePatternId) {
                    IUIAutomationInvokePattern* pattern = nullptr;
                    action_hr = unknown->QueryInterface(IID_PPV_ARGS(&pattern));
                    if (SUCCEEDED(action_hr) && pattern) {
                        action_hr = pattern->Invoke();
                        pattern->Release();
                    }
                } else if (pattern_id == UIA_TogglePatternId) {
                    IUIAutomationTogglePattern* pattern = nullptr;
                    action_hr = unknown->QueryInterface(IID_PPV_ARGS(&pattern));
                    if (SUCCEEDED(action_hr) && pattern) {
                        action_hr = pattern->Toggle();
                        pattern->Release();
                    }
                } else if (pattern_id == UIA_SelectionItemPatternId) {
                    IUIAutomationSelectionItemPattern* pattern = nullptr;
                    action_hr = unknown->QueryInterface(IID_PPV_ARGS(&pattern));
                    if (SUCCEEDED(action_hr) && pattern) {
                        action_hr = pattern->Select();
                        pattern->Release();
                    }
                } else {
                    IUIAutomationExpandCollapsePattern* pattern = nullptr;
                    action_hr = unknown->QueryInterface(IID_PPV_ARGS(&pattern));
                    if (SUCCEEDED(action_hr) && pattern) {
                        action_hr = (op == "expand") ? pattern->Expand() : pattern->Collapse();
                        pattern->Release();
                    }
                }
                unknown->Release();
            }
            ok = SUCCEEDED(action_hr);
        } else if (result) {
            *result = "unsupported UI Automation action";
        }
    }

    target->Release();
    if (uninitialize) CoUninitialize();
    if (!ok && result && result->empty()) {
        if (action_hr == UIA_E_ELEMENTNOTAVAILABLE)
            *result = "the control disappeared before the action completed";
        else if (action_hr == E_NOINTERFACE)
            *result = "the control does not support this action pattern";
        else
            *result = "Windows UI Automation action failed (HRESULT " +
                      std::to_string((unsigned long)action_hr) + ")";
    } else if (ok && result && result->empty()) {
        *result = op + " succeeded";
    }
    return ok;
}

}  // namespace hawk
