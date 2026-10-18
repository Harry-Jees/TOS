// Hawk OS - explicit, user-started mouse/keyboard demonstration recorder.
// Keystrokes are stored as replayable key chords in the local workflow file.
// Start only when ready; stop before entering passwords or other secrets.
#include "hawk.h"
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace hawk {
namespace {
std::mutex g_lock;
std::condition_variable g_ready;
std::vector<std::string> g_events;
std::thread g_thread;
DWORD g_thread_id=0;
HHOOK g_mouse=nullptr,g_keyboard=nullptr;
std::atomic<bool> g_active{false};
bool g_ready_flag=false,g_setup_ok=false;
std::string g_setup_error;
POINT g_left_down{}; bool g_left_pending=false;

void add_event(const std::string& event) {
    std::lock_guard<std::mutex> guard(g_lock);
    if(!g_active.load()||g_events.size()>=20000)return;
    if(event.size()+1>1024*1024)return;
    g_events.push_back(event);
}
std::string key_name(DWORD vk) {
    if(vk>='A'&&vk<='Z')return std::string(1,(char)std::tolower((unsigned char)vk));
    if(vk>='0'&&vk<='9')return std::string(1,(char)vk);
    if(vk>=VK_F1&&vk<=VK_F24)return "f"+std::to_string(vk-VK_F1+1);
    switch(vk) {
    case VK_RETURN:return "enter";case VK_TAB:return "tab";case VK_ESCAPE:return "esc";
    case VK_SPACE:return "space";case VK_BACK:return "backspace";case VK_DELETE:return "delete";
    case VK_INSERT:return "insert";case VK_HOME:return "home";case VK_END:return "end";
    case VK_PRIOR:return "pageup";case VK_NEXT:return "pagedown";case VK_LEFT:return "left";
    case VK_RIGHT:return "right";case VK_UP:return "up";case VK_DOWN:return "down";
    case VK_CAPITAL:return "capslock";case VK_SNAPSHOT:return "printscreen";
    default:return {};
    }
}
LRESULT CALLBACK mouse_hook(int code,WPARAM wp,LPARAM lp) {
    if(code==HC_ACTION&&g_active.load()) {
        const auto* m=reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        if(wp==WM_LBUTTONDOWN){g_left_down=m->pt;g_left_pending=true;}
        else if(wp==WM_LBUTTONUP&&g_left_pending){
            g_left_pending=false;
            if(abs(m->pt.x-g_left_down.x)>4||abs(m->pt.y-g_left_down.y)>4)
                add_event("drag "+std::to_string(g_left_down.x)+" "+std::to_string(g_left_down.y)+" "+std::to_string(m->pt.x)+" "+std::to_string(m->pt.y));
            else add_event("click "+std::to_string(m->pt.x)+" "+std::to_string(m->pt.y)+" left");
        } else if(wp==WM_RBUTTONDOWN)add_event("click "+std::to_string(m->pt.x)+" "+std::to_string(m->pt.y)+" right");
        else if(wp==WM_MBUTTONDOWN)add_event("click "+std::to_string(m->pt.x)+" "+std::to_string(m->pt.y)+" middle");
    }
    return CallNextHookEx(g_mouse,code,wp,lp);
}
LRESULT CALLBACK keyboard_hook(int code,WPARAM wp,LPARAM lp) {
    if(code==HC_ACTION&&g_active.load()&&(wp==WM_KEYDOWN||wp==WM_SYSKEYDOWN)) {
        const auto* k=reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        const DWORD vk=k->vkCode;
        if(vk!=VK_SHIFT&&vk!=VK_LSHIFT&&vk!=VK_RSHIFT&&vk!=VK_CONTROL&&vk!=VK_LCONTROL&&vk!=VK_RCONTROL&&
           vk!=VK_MENU&&vk!=VK_LMENU&&vk!=VK_RMENU&&vk!=VK_LWIN&&vk!=VK_RWIN) {
            std::string key=key_name(vk);
            if(!key.empty()) {
                std::string chord;
                if(GetAsyncKeyState(VK_CONTROL)&0x8000)chord+="ctrl+";
                if(GetAsyncKeyState(VK_MENU)&0x8000)chord+="alt+";
                if(GetAsyncKeyState(VK_SHIFT)&0x8000)chord+="shift+";
                if((GetAsyncKeyState(VK_LWIN)&0x8000)||(GetAsyncKeyState(VK_RWIN)&0x8000))chord+="win+";
                chord+=key;
                add_event("key "+chord);
            }
        }
    }
    return CallNextHookEx(g_keyboard,code,wp,lp);
}
void hook_thread() {
    MSG dummy; PeekMessage(&dummy,nullptr,WM_USER,WM_USER,PM_NOREMOVE); // create this thread's message queue
    g_thread_id=GetCurrentThreadId();
    g_mouse=SetWindowsHookExW(WH_MOUSE_LL,mouse_hook,GetModuleHandleW(nullptr),0);
    g_keyboard=SetWindowsHookExW(WH_KEYBOARD_LL,keyboard_hook,GetModuleHandleW(nullptr),0);
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_setup_ok=g_mouse&&g_keyboard;
        if(!g_setup_ok)g_setup_error="Windows refused the low-level input hooks; recording was not started";
        g_ready_flag=true;
    }
    g_ready.notify_one();
    if(g_setup_ok)while(GetMessageW(&dummy,nullptr,0,0)>0){TranslateMessage(&dummy);DispatchMessageW(&dummy);}
    if(g_mouse){UnhookWindowsHookEx(g_mouse);g_mouse=nullptr;}
    if(g_keyboard){UnhookWindowsHookEx(g_keyboard);g_keyboard=nullptr;}
    g_active=false;
}
}
bool start_input_recording(std::string* error) {
    if(g_active.load()||g_thread.joinable()){if(error)*error="input recording is already active";return false;}
    {std::lock_guard<std::mutex> guard(g_lock);g_events.clear();g_ready_flag=false;g_setup_ok=false;g_setup_error.clear();}
    g_active=true;
    try {g_thread=std::thread(hook_thread);} catch(...) {g_active=false;if(error)*error="could not create recorder thread";return false;}
    std::unique_lock<std::mutex> lock(g_lock);
    g_ready.wait_for(lock,std::chrono::seconds(3),[]{return g_ready_flag;});
    if(!g_ready_flag||!g_setup_ok) {
        std::string why=g_ready_flag?g_setup_error:"input recorder initialization timed out";
        lock.unlock();g_active=false;if(g_thread_id)PostThreadMessageW(g_thread_id,WM_QUIT,0,0);if(g_thread.joinable())g_thread.join();
        if(error)*error=why;return false;
    }
    return true;
}
bool stop_input_recording(std::string* script,std::string* error) {
    if(!g_thread.joinable()){if(error)*error="input recording is not active";return false;}
    g_active=false;if(g_thread_id)PostThreadMessageW(g_thread_id,WM_QUIT,0,0);g_thread.join();
    std::lock_guard<std::mutex> guard(g_lock);
    if(g_events.empty()){if(error)*error="recording contained no supported mouse clicks or key presses";return false;}
    script->clear();
    for(const auto& e:g_events){if(script->size()+e.size()+1>1024*1024){if(error)*error="recording exceeds the 1 MiB workflow limit";return false;}*script+=e+"\n";}
    return true;
}
bool input_recording_active(){return g_active.load();}
} // namespace hawk
