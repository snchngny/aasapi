#pragma once
#include "port_inventory.h"
#include <commctrl.h>

namespace aggregate {
// The panel edits a candidate; the owner alone stops and reconfigures the engine.
class SettingsPanel {
    struct Master {std::wstring name;std::string clsid;};
    std::vector<Master> masters_;
    std::vector<WindowsPort> ports_;
    std::vector<HWND> controls_;
    HWND master_,rate_,frames_,queue_,capture_,playback_;
    Config saved_;
    static std::wstring value(HWND control) {int n=GetWindowTextLengthW(control);std::wstring text(n+1,L'\0');GetWindowTextW(control,text.data(),n+1);text.resize(n);return text;}
public:
    enum {apply_id=500,refresh_id,panel_id};
    void create(HWND parent,HFONT font,const Config& config) {
        auto add=[&](const wchar_t* kind,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id=0) {
            auto control=CreateWindowExW(0,kind,text,WS_CHILD|style,x,y,w,h,parent,reinterpret_cast<HMENU>(INT_PTR(id)),nullptr,nullptr);
            if(!control) throw std::runtime_error("Cannot create settings panel");
            SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);controls_.push_back(control);return control;
        };
        add(L"STATIC",L"主ASIO（この機器のクロックと全入出力を使用）",0,20,202,420,24);
        master_=add(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,20,228,550,260);
        add(L"BUTTON",L"メーカー設定",WS_TABSTOP,590,228,150,28,panel_id);
        add(L"STATIC",L"サンプルレート [Hz]",0,20,268,240,24);rate_=add(L"EDIT",L"",WS_BORDER|ES_NUMBER|WS_TABSTOP,280,265,140,28);
        add(L"STATIC",L"ブロック長 [frames]",0,20,306,240,24);frames_=add(L"EDIT",L"",WS_BORDER|ES_NUMBER|WS_TABSTOP,280,303,140,28);
        add(L"STATIC",L"追加機器の待ちブロック数",0,20,344,250,24);queue_=add(L"EDIT",L"",WS_BORDER|ES_NUMBER|WS_TABSTOP,280,341,140,28);
        add(L"STATIC",L"出力ごとの遅延調整・マイクによる自動計測は「遅延補正」タブ",0,20,382,880,48);
        add(L"STATIC",L"DAW公開用の追加入力／出力（ルーティング画面は選択に関係なく全ポートを表示）",0,20,448,880,24);
        capture_=add(WC_LISTVIEWW,L"DAW公開用の追加入力",WS_BORDER|LVS_REPORT|WS_TABSTOP,20,478,430,120);
        playback_=add(WC_LISTVIEWW,L"DAW公開用の追加出力",WS_BORDER|LVS_REPORT|WS_TABSTOP,470,478,430,120);
        for(auto list:{capture_,playback_}) {ListView_SetExtendedListViewStyle(list,LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);LVCOLUMNW col{};col.mask=LVCF_TEXT|LVCF_WIDTH;col.pszText=const_cast<wchar_t*>(L"機器名 / ch");col.cx=400;ListView_InsertColumn(list,0,&col);}
        add(L"BUTTON",L"設定を適用",WS_TABSTOP,20,612,170,30,apply_id);
        add(L"BUTTON",L"機器一覧を更新",WS_TABSTOP,210,612,180,30,refresh_id);
        add(L"STATIC",L"適用時は一時停止して再初期化し、元の運転・ミュート状態へ戻る",0,20,652,880,24);
        refresh(config);
    }
    void show(bool visible) {for(auto c:controls_) ShowWindow(c,visible?SW_SHOW:SW_HIDE);}
    void enable(bool enabled) {for(auto c:controls_) EnableWindow(c,enabled);}
    void refresh(const Config& config) {
        saved_=config;masters_.clear();masters_.push_back({L"主ASIOを選択してください",{}});
        HKEY key=nullptr;
        if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\ASIO",0,KEY_READ|KEY_WOW64_64KEY,&key)==ERROR_SUCCESS) {
            wchar_t name[256];DWORD size=256;
            for(DWORD i=0;RegEnumKeyExW(key,i,name,&size,nullptr,nullptr,nullptr,nullptr)==ERROR_SUCCESS;++i,size=256) {
                HKEY child=nullptr;wchar_t clsid[256]{};DWORD bytes=sizeof(clsid),type=0;
                if(RegOpenKeyExW(key,name,0,KEY_READ,&child)==ERROR_SUCCESS) {
                    if(RegQueryValueExW(child,L"CLSID",nullptr,&type,reinterpret_cast<BYTE*>(clsid),&bytes)==ERROR_SUCCESS&&type==REG_SZ&&clsid[0]&&_wcsicmp(clsid,wide(aggregate_clsid_text).c_str())!=0) masters_.push_back({name,utf8(clsid)});
                    RegCloseKey(child);
                }
            }RegCloseKey(key);
        }
        if(!config.native_asio_clsid.empty()&&std::none_of(masters_.begin(),masters_.end(),[&](auto& m){return m.clsid==config.native_asio_clsid;})) masters_.push_back({L"未接続の主ASIO / "+wide(config.native_asio_clsid),config.native_asio_clsid});
        SendMessageW(master_,CB_RESETCONTENT,0,0);unsigned selected=0;
        for(unsigned i=0;i<masters_.size();++i) {SendMessageW(master_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(masters_[i].name.c_str()));if(masters_[i].clsid==config.native_asio_clsid) selected=i;}SendMessageW(master_,CB_SETCURSEL,selected,0);
        SetWindowTextW(rate_,std::to_wstring(config.sample_rate).c_str());SetWindowTextW(frames_,std::to_wstring(config.block_frames).c_str());SetWindowTextW(queue_,std::to_wstring(config.queue_blocks).c_str());
        ports_=windows_ports();
        for(bool input:{true,false}) {
            auto& specs=input?config.capture:config.playback;auto list=input?capture_:playback_;ListView_DeleteAllItems(list);
            for(auto& spec:specs) if(std::none_of(ports_.begin(),ports_.end(),[&](auto& p){return p.input==input&&p.spec.id==spec.id;})) ports_.push_back({spec,"未接続 / "+spec.id,input});
            unsigned row=0;for(auto& port:ports_) if(port.input==input) {std::wstring label=wide(port.name)+L" / "+std::to_wstring(port.spec.channels)+L" ch";LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=row;item.pszText=label.data();ListView_InsertItem(list,&item);ListView_SetCheckState(list,row,std::any_of(specs.begin(),specs.end(),[&](auto& s){return s.id==port.spec.id;}));++row;}
        }
    }
    std::filesystem::path candidate(const std::filesystem::path& path) {
        int index=int(SendMessageW(master_,CB_GETCURSEL,0,0));if(index<=0) throw std::runtime_error("主ASIOを選択してください");
        std::ostringstream text;text<<"[engine]\nsample_rate="<<utf8(value(rate_).c_str())<<"\nblock_frames="<<utf8(value(frames_).c_str())<<"\nqueue_blocks="<<utf8(value(queue_).c_str())<<"\npc_channels="<<saved_.pc_channels<<"\noutput_delay_ms=";
        for(unsigned i=0;i<saved_.output_delay_ms.size();++i) {if(i) text<<',';text<<saved_.output_delay_ms[i];}
        text<<"\nexclusive=false\nsimulation=false\nnative_asio_clsid="<<masters_.at(index).clsid<<"\n";
        for(bool input:{true,false}) {unsigned row=0,n=0;for(auto& port:ports_) if(port.input==input) {if(ListView_GetCheckState(input?capture_:playback_,row)) text<<"\n["<<(input?"capture":"playback")<<'.'<<n++<<"]\nid="<<port.spec.id<<"\nchannels="<<port.spec.channels<<"\n";++row;}}
        {std::ofstream out(path);out<<text.str();if(!out) throw std::runtime_error("Cannot write settings candidate");}load_config(path);return path;
    }
    void control_panel() {
        int index=int(SendMessageW(master_,CB_GETCURSEL,0,0));if(index<=0) throw std::runtime_error("主ASIOを選択してください");
        NativeAsio driver(masters_.at(index).clsid,256,48000,true);driver.control_panel();
    }
    void self_test(const std::filesystem::path& path) {
        auto initial=value(rate_);SetWindowTextW(rate_,L"0");bool rejected=false;
        try {candidate(path);}catch(const std::exception&) {rejected=true;}SetWindowTextW(rate_,initial.c_str());
        if(!rejected) throw std::runtime_error("Invalid sample rate accepted");
        auto actual=load_config(candidate(path));
        if(actual.sample_rate!=saved_.sample_rate || actual.block_frames!=saved_.block_frames || actual.queue_blocks!=saved_.queue_blocks || actual.native_asio_clsid!=saved_.native_asio_clsid || actual.output_delay_ms!=saved_.output_delay_ms || actual.capture.size()!=saved_.capture.size() || actual.playback.size()!=saved_.playback.size()) throw std::runtime_error("Settings round-trip mismatch");
    }
};
}
