// Hawk OS - explicit localhost Chrome DevTools Protocol adapter.
// The adapter never connects to a remote host: CDP is a privileged browser control
// interface and is intentionally pinned to 127.0.0.1:9222.
#include "hawk.h"
#include <winhttp.h>
#include <algorithm>
#include <cstdio>
#include <string>

namespace hawk {
namespace {
std::string json_quote(const std::string& s) {
    std::string o="\"";
    for(unsigned char c:s) {
        switch(c) {
        case '\"': o+="\\\""; break; case '\\': o+="\\\\"; break;
        case '\b': o+="\\b"; break; case '\f': o+="\\f"; break;
        case '\n': o+="\\n"; break; case '\r': o+="\\r"; break; case '\t': o+="\\t"; break;
        default: if(c<0x20) { char b[7]; snprintf(b,sizeof(b),"\\u%04x",c); o+=b; } else o+=(char)c;
        }
    }
    o+='\"'; return o;
}
std::string json_string_after(const std::string& s,const std::string& key) {
    size_t p=s.find(key); if(p==std::string::npos)return {};
    p=s.find(':',p+key.size()); if(p==std::string::npos)return {};
    p=s.find('"',p+1); if(p==std::string::npos)return {};
    std::string out; bool esc=false;
    for(++p;p<s.size();++p) { char c=s[p]; if(esc){switch(c){case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;default:out+=c;}esc=false;}
        else if(c=='\\')esc=true; else if(c=='"')return out; else out+=c; }
    return {};
}
bool read_response(HINTERNET req,std::string* out,std::string* err) {
    out->clear(); DWORD available=0;
    while(WinHttpQueryDataAvailable(req,&available) && available) {
        if(out->size()+available>2*1024*1024){*err="CDP response exceeded 2 MiB";return false;}
        std::string chunk(available,'\0'); DWORD got=0;
        if(!WinHttpReadData(req,&chunk[0],available,&got)){*err="failed reading local CDP response";return false;}
        chunk.resize(got);*out+=chunk;available=0;
    }
    return true;
}
bool get_page_socket(std::string* url,std::string* err) {
    HINTERNET session=WinHttpOpen(L"HawkOS-CDP/1.0",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session){*err="WinHTTP session failed";return false;}
    HINTERNET conn=WinHttpConnect(session,L"127.0.0.1",9222,0);
    HINTERNET req=conn?WinHttpOpenRequest(conn,L"GET",L"/json",nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0):nullptr;
    bool ok=req&&WinHttpSendRequest(req,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(req,nullptr);
    std::string body;
    if(!ok)*err="cannot reach Chrome DevTools at 127.0.0.1:9222; start the browser with remote debugging enabled";
    else ok=read_response(req,&body,err);
    if(req)WinHttpCloseHandle(req);if(conn)WinHttpCloseHandle(conn);WinHttpCloseHandle(session);
    if(!ok)return false;
    size_t type=body.find("\"type\":\"page\"");
    size_t p=type==std::string::npos?body.find("\"webSocketDebuggerUrl\""):body.find("\"webSocketDebuggerUrl\"",type);
    if(p==std::string::npos){*err="no debuggable browser page found on localhost CDP";return false;}
    size_t q=body.find('"',body.find(':',p)+1);if(q==std::string::npos){*err="invalid CDP target response";return false;}
    size_t e=body.find('"',q+1);if(e==std::string::npos){*err="invalid CDP websocket URL";return false;}
    *url=body.substr(q+1,e-q-1);
    if(url->rfind("ws://127.0.0.1:",0)!=0 && url->rfind("ws://localhost:",0)!=0){*err="refusing non-local CDP websocket";return false;}
    return true;
}
bool cdp_call(const std::string& method,const std::string& params,std::string* response,std::string* err) {
    std::string ws;if(!get_page_socket(&ws,err))return false;
    std::string http_url=ws; if(http_url.rfind("ws://",0)==0) http_url.replace(0,5,"http://");
    std::wstring wurl=wide(http_url);URL_COMPONENTS uc{};uc.dwStructSize=sizeof(uc);
    wchar_t host[256]{},path[2048]{};uc.lpszHostName=host;uc.dwHostNameLength=256;uc.lpszUrlPath=path;uc.dwUrlPathLength=2048;
    if(!WinHttpCrackUrl(wurl.c_str(),0,0,&uc)){*err="invalid local CDP websocket URL";return false;}
    HINTERNET session=WinHttpOpen(L"HawkOS-CDP/1.0",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session){*err="WinHTTP session failed";return false;}
    HINTERNET conn=WinHttpConnect(session,host,uc.nPort,0);
    HINTERNET req=conn?WinHttpOpenRequest(conn,L"GET",uc.lpszUrlPath,nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0):nullptr;
    bool ok=req!=nullptr; DWORD upgrade=WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET;
    if(ok)ok=WinHttpSetOption(req,upgrade,nullptr,0)&&WinHttpSendRequest(req,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(req,nullptr);
    HINTERNET socket=ok?WinHttpWebSocketCompleteUpgrade(req,0):nullptr;
    if(req)WinHttpCloseHandle(req);if(!socket){if(ok)*err="CDP websocket upgrade failed (Chrome may require --remote-allow-origins=*)";else *err="CDP websocket connection failed";if(conn)WinHttpCloseHandle(conn);WinHttpCloseHandle(session);return false;}
    static volatile LONG sequence=0;long id=InterlockedIncrement(&sequence);
    std::string msg="{\"id\":"+std::to_string(id)+",\"method\":"+json_quote(method)+",\"params\":"+(params.empty()?"{}":params)+"}";
    ok=WinHttpWebSocketSend(socket,WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,(PVOID)msg.data(),(DWORD)msg.size())==NO_ERROR;
    std::string reply;
    for(int attempt=0;ok&&attempt<12;++attempt){
        char buf[16384];DWORD got=0;WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        if(WinHttpWebSocketReceive(socket,buf,sizeof(buf),&got,&type)!=NO_ERROR){ok=false;break;}
        reply.append(buf,got);
        if(type==WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE){ok=false;break;}
        if(type==WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE){
            if(reply.find("\"id\":"+std::to_string(id))!=std::string::npos)break;
            reply.clear();
        }
    }
    if(!ok)*err="CDP websocket request failed";
    else if(reply.empty()||reply.find("\"id\":"+std::to_string(id))==std::string::npos){*err="CDP command timed out or returned no matching response";ok=false;}
    if(ok)*response=reply;
    WinHttpWebSocketClose(socket,WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,nullptr,0);WinHttpCloseHandle(socket);if(conn)WinHttpCloseHandle(conn);WinHttpCloseHandle(session);
    return ok;
}
bool evaluate(const std::string& expression,std::string* response,std::string* err) {
    std::string params="{\"expression\":"+json_quote(expression)+",\"returnByValue\":true,\"awaitPromise\":true}";
    if(!cdp_call("Runtime.evaluate",params,response,err))return false;
    if(response->find("\"exceptionDetails\"")!=std::string::npos){*err="browser expression failed; selector or page state may be invalid";return false;}
    return true;
}
}
bool browser_command(const std::vector<std::string>& a,bool dry_run,std::string* result,std::string* error) {
    if(a.size()<2){*error="usage: browser status|text <selector>|click <selector>|fill <selector> <text>|navigate <http(s)-url>";return false;}
    const std::string op=to_lower(a[1]);
    if(op=="status") {
        std::string url;if(!get_page_socket(&url,error))return false;
        *result="local CDP connected; page target available";return true;
    }
    if(op=="navigate") {
        if(a.size()!=3 || (a[2].rfind("https://",0)!=0&&a[2].rfind("http://",0)!=0)){*error="browser navigate requires one http:// or https:// URL";return false;}
        if(dry_run){*result="dry-run: would navigate browser to "+a[2];return true;}
        std::string reply;if(!cdp_call("Page.navigate","{\"url\":"+json_quote(a[2])+"}",&reply,error))return false;
        *result="browser navigation requested";return true;
    }
    if(op!="text"&&op!="click"&&op!="fill"){*error="unsupported browser operation";return false;}
    if((op=="text"&&a.size()!=3)||(op=="click"&&a.size()!=3)||(op=="fill"&&a.size()!=4)){*error="usage: browser text <css-selector> | click <css-selector> | fill <css-selector> <text>";return false;}
    const std::string selector=json_quote(a[2]);
    std::string expression;
    if(op=="text") expression="(()=>{const e=document.querySelector("+selector+");return e?(e.innerText||e.textContent||'').slice(0,4000):null})()";
    else if(op=="click") expression="(()=>{const e=document.querySelector("+selector+");if(!e)return false;e.click();return true})()";
    else expression="(()=>{const e=document.querySelector("+selector+");if(!e)return false;e.focus();e.value="+json_quote(a[3])+";e.dispatchEvent(new Event('input',{bubbles:true}));e.dispatchEvent(new Event('change',{bubbles:true}));return true})()";
    if(dry_run){*result="dry-run: browser "+op+" "+a[2];return true;}
    std::string reply;if(!evaluate(expression,&reply,error))return false;
    if(reply.find("\"value\":false")!=std::string::npos||reply.find("\"value\":null")!=std::string::npos){*error="browser selector did not match an element";return false;}
    if(op=="text") { *result=json_string_after(reply,"\"value\""); if(result->empty())*result="(no text returned)"; }
    else *result="browser "+op+" completed";
    return true;
}
} // namespace hawk
