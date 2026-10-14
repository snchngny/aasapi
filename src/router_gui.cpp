#include "router_session.h"
#include "device_refresh.h"
#include "device_watcher.h"
#include "settings_panel.h"
#include "delay_panel.h"
#include "calibration_session.h"
#include <commctrl.h>
#include <oleacc.h>
#include <shellapi.h>
#include <iomanip>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace {
constexpr UINT update_message=WM_APP+10;
constexpr UINT tray_message=WM_APP+11;
constexpr int tray_open_id=400,tray_exit_id=401,resident_id=402;
constexpr int cell_base=1000,row_width=300,column_width=130,header_height=76,row_height=34;
enum {source_id=200,gain_id,mute_id,tone_id,save_id,start_id,stop_id,clear_id,all_outputs_id};
struct Update {bool final=false; unsigned health=0; std::string text,error;bool calibrated=false;aggregate::CalibrationResult calibration;};
HWND window,pane,gain_control,mute_control,tone_control,start_control,stop_control,status_control,report_control,gain_label;
HFONT font;
IAccPropServices* accessibility=nullptr;
std::filesystem::path root;
aggregate::Config config;
std::vector<aggregate::RouteChannel> input_channels,output_channels;
std::unique_ptr<aggregate::RoutingMatrix> matrix;
std::vector<HWND> cells,row_labels,column_labels,spare_cells;
HWND corner;
HWND tabs,clear_control,all_control,diagnostic_note,simulation_control;
HWND silent_control;
aggregate::SettingsPanel settings;
aggregate::DelayPanel delay_panel;
aggregate::DeviceWatcher device_watcher;
aggregate::PortDelays port_delays;
bool calibration_running=false,calibration_pending=false,resume_after_calibration=false;
unsigned calibration_input=0;
float calibration_gain=1;
std::vector<unsigned> calibration_outputs;
int selected_tab=0;
bool applying=false,resume_after_apply=false,diagnostic_running=false;
bool simulation_running=false;
bool diagnostic_previous_mute=true;
std::filesystem::path pending_settings;
void start();
void rebuild_grid();
int scroll_x=0,scroll_y=0;
std::thread worker;
HANDLE stop_event=nullptr;
bool busy=false,closing=false;
bool resident=false,resident_running=false,resident_muted=true,resident_test=false;
HWND resident_control;
NOTIFYICONDATAW tray{};
bool all_outputs=true;
std::string startup_note;
struct Placement {int x,y,w,h;bool visible;};
std::unordered_map<HWND,Placement> placements;
std::unordered_map<HWND,std::wstring> displayed_text;
void changed_text(HWND control,const std::wstring& text) {
    auto& previous=displayed_text[control];if(previous==text) return;previous=text;SetWindowTextW(control,text.c_str());
}

HWND make(HWND parent,const wchar_t* kind,const std::wstring& text,DWORD style,int x,int y,int w,int h,int id=0) {
    HWND c=CreateWindowExW(0,kind,text.c_str(),WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS|style,x,y,w,h,parent,reinterpret_cast<HMENU>(INT_PTR(id)),nullptr,nullptr);
    if(!c) throw std::runtime_error("Cannot create routing control");
    SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE); return c;
}
std::vector<aggregate::EndpointSpec> loopback_specs;
unsigned loopback_channels=0;
std::vector<std::pair<std::string,std::string>> unresolved_routes;
void inventory() {
    {std::ifstream saved(root/L"config/router.txt");std::string line;
    while(std::getline(saved,line)) {std::istringstream fields(line);std::string kind;fields>>kind;
        if(kind=="port") {std::string direction,id;unsigned channels=0;fields>>direction>>std::quoted(id)>>channels;if(id.empty()||channels<1||channels>64||(direction!="input"&&direction!="output")) throw std::runtime_error("Invalid saved port");auto& list=direction=="input"?config.capture:config.playback;if(std::none_of(list.begin(),list.end(),[&](auto& p){return p.id==id;})) list.push_back({id,channels});}
        else if(kind=="source") {std::string id;fields>>std::quoted(id);if(!id.empty()&&std::none_of(config.playback.begin(),config.playback.end(),[&](auto& p){return p.id==id;})) config.playback.push_back({id,config.pc_channels});}
    }}
    auto ports=aggregate::windows_ports();aggregate::include_windows_ports(config,ports);
    input_channels.clear();output_channels.clear();loopback_specs.clear();
    for(auto& spec:config.playback) {
        auto found=std::find_if(ports.begin(),ports.end(),[&](auto& p){return !p.input&&p.spec.id==spec.id;});
        std::string name=found==ports.end()?"未接続 / "+spec.id:found->name;
        loopback_specs.push_back(spec);
        for(unsigned i=0;i<spec.channels;++i) input_channels.push_back({aggregate::loopback_key(spec.id,i),"Windows再生音 / "+name+" / "+std::to_string(i+1),spec.id,{}});
    }
    loopback_channels=unsigned(input_channels.size());
    if(!loopback_channels) {loopback_channels=2;loopback_specs.push_back({"",2});input_channels.push_back({"pc:L","PC音 未接続 L",{}, {}});input_channels.push_back({"pc:R","PC音 未接続 R",{}, {}});}
    if(!config.native_asio_clsid.empty()) {
        aggregate::NativeAsio native(config.native_asio_clsid,config.block_frames,config.sample_rate);
        for(bool input:{true,false}) for(unsigned i=0;i<(input?native.inputs():native.outputs());++i)
            (input?input_channels:output_channels).push_back({"asio:"+config.native_asio_clsid+(input?":in:":":out:")+std::to_string(i),"ASIO "+std::to_string(i+1)+" / "+native.channel_name(input,i),{}, {}});
    }
    for(bool input:{true,false}) for(auto& spec:input?config.capture:config.playback) {
        auto found=std::find_if(ports.begin(),ports.end(),[&](auto& p){return p.input==input&&p.spec.id==spec.id;});
        std::string name=found==ports.end()?"未接続 / "+spec.id:found->name;
        for(unsigned i=0;i<spec.channels;++i) (input?input_channels:output_channels).push_back({"wasapi:"+spec.id+":"+std::to_string(i),"Windows "+std::string(input?"入力 / ":"出力 / ")+name+" / "+std::to_string(i+1),spec.id,{}});
    }
    matrix=std::make_unique<aggregate::RoutingMatrix>(unsigned(input_channels.size()),unsigned(output_channels.size()),loopback_channels);
    std::filesystem::create_directories(root/L"build/results");std::ofstream listing(root/L"build/results/router-ports.txt");
    for(auto& channel:input_channels) listing<<"input "<<std::quoted(channel.key)<<" "<<channel.name<<"\n";
    for(auto& channel:output_channels) listing<<"output "<<std::quoted(channel.key)<<" "<<channel.name<<"\n";
}
void save_routes() {
    if(resident_test) return;
    auto candidate=root/L"build/router-candidate.txt";std::ofstream out(candidate);out<<"version 2\ngain "<<matrix->gain.load()<<"\n";
    for(bool input:{true,false}) for(auto& spec:input?config.capture:config.playback) out<<"port "<<(input?"input":"output")<<' '<<std::quoted(spec.id)<<' '<<spec.channels<<"\n";
    for(unsigned r=0;r<input_channels.size();++r) for(unsigned c=0;c<output_channels.size();++c)
        if(matrix->requested(r,c)) out<<"route "<<std::quoted(input_channels[r].key)<<' '<<std::quoted(output_channels[c].key)<<"\n";
    for(auto& [a,b]:unresolved_routes) out<<"route "<<std::quoted(a)<<' '<<std::quoted(b)<<"\n";
    aggregate::write_delays(out,port_delays);delay_panel.save_selection(out);
    out.close();if(!out) throw std::runtime_error("Cannot write routing settings");
    if(!MoveFileExW(candidate.c_str(),(root/L"config/router.txt").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot save routing settings");
}
void save_resident() {
    if(resident_test) return;
    auto candidate=root/L"build/resident-candidate.txt";
    {std::ofstream out(candidate);out<<resident<<' '<<resident_running<<' '<<matrix->muted.load()<<'\n';if(!out) throw std::runtime_error("Cannot save resident state");}
    if(!MoveFileExW(candidate.c_str(),(root/L"config/resident.txt").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot replace resident state");
}
void register_startup(bool enabled) {
    HKEY key=nullptr;
    auto result=RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
    if(result!=ERROR_SUCCESS) throw std::runtime_error("Cannot open user startup settings");
    if(enabled) {
        wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
        std::wstring command=L"\""+std::wstring(executable)+L"\" --background";
        result=RegSetValueExW(key,L"AudioAggregateRouter",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),DWORD((command.size()+1)*sizeof(wchar_t)));
    } else {result=RegDeleteValueW(key,L"AudioAggregateRouter");if(result==ERROR_FILE_NOT_FOUND) result=ERROR_SUCCESS;}
    RegCloseKey(key);if(result!=ERROR_SUCCESS) throw std::runtime_error("Cannot update user startup settings");
}
void show_router() {ShowWindow(window,SW_RESTORE);SetForegroundWindow(window);}
void request_exit() {
    closing=true;
    if(busy) {SetEvent(stop_event);SetWindowTextW(status_control,L"音声を停止して終了…");}
    else DestroyWindow(window);
}
void load_routes() {
    std::ifstream in(root/L"config/router.txt");std::string source,line,kind;unsigned missing=0;std::vector<std::pair<std::string,std::string>> saved;
    port_delays.clear();bool keyed_delays=false;
    while(std::getline(in,line)) {
        std::istringstream fields(line);fields>>kind;
        if(kind=="version") {int version=0;fields>>version;if(version!=1&&version!=2) throw std::runtime_error("Unsupported routing file version");}
        else if(kind=="delay_version") {unsigned version=0;if(!(fields>>version)||version!=1) throw std::runtime_error("Unsupported delay settings version");keyed_delays=true;}
        else if(kind=="delay") {std::string key,value;if(!(fields>>std::quoted(key)>>value)||key.empty()) throw std::runtime_error("Invalid saved delay");if(!port_delays.emplace(key,aggregate::valid_delay(value)).second) throw std::runtime_error("Duplicate saved delay");}
        else if(kind=="port") {}
        else if(kind=="calibration_mic"||kind=="calibration_output") {std::string key;if(!(fields>>std::quoted(key))||key.empty()) throw std::runtime_error("Invalid calibration selection");}
        else if(kind=="source") fields>>std::quoted(source);
        else if(kind=="gain") {float gain=0;if(!(fields>>gain)||!std::isfinite(gain)||gain<0||gain>1) throw std::runtime_error("Invalid saved gain");matrix->gain=gain;}
        else if(kind=="route") {std::string a,b;if(!(fields>>std::quoted(a)>>std::quoted(b))) throw std::runtime_error("Invalid saved route");saved.push_back({a,b});}
        else if(!kind.empty()) throw std::runtime_error("Unknown routing setting");
    }
    if(!keyed_delays) for(unsigned c=0;c<std::min(output_channels.size(),config.output_delay_ms.size());++c) port_delays.try_emplace(output_channels[c].key,config.output_delay_ms[c]);
    for(auto [a,b]:saved) {
        if(a=="pc:L"||a=="pc:R"||a.rfind("pc:",0)==0) {unsigned n=a=="pc:L"?0:a=="pc:R"?1:unsigned(std::stoul(a.substr(3)));a=aggregate::loopback_key(source,n);}
        auto r=std::find_if(input_channels.begin(),input_channels.end(),[&](auto& x){return x.key==a;});auto c=std::find_if(output_channels.begin(),output_channels.end(),[&](auto& x){return x.key==b;});
        if(r==input_channels.end()||c==output_channels.end()) {++missing;unresolved_routes.push_back({a,b});}else matrix->connect(unsigned(r-input_channels.begin()),unsigned(c-output_channels.begin()),true);
    }
    if(missing) startup_note=" / 読込対象のない設定 "+std::to_string(missing)+"件（未解決の経路も保存時に保持）";
}
std::wstring db(float peak) { if(peak<.00001f) return L"−∞"; wchar_t text[32]{}; swprintf_s(text,L"%.1f",20*std::log10(peak)); return text; }
void layout_pane() {
    if(!matrix||selected_tab!=0) return;
    RECT rect{}; GetClientRect(pane,&rect);
    std::vector<int> positions(output_channels.size(),-1); int visible_count=0;
    for(unsigned c=0;c<output_channels.size();++c) if(all_outputs || matrix->output_connected(c)) positions[c]=visible_count++;
    std::vector<int> rows(input_channels.size(),-1); int visible_rows=0;
    for(unsigned r=0;r<input_channels.size();++r) if(all_outputs || r<loopback_channels || matrix->input_connected(r)) rows[r]=visible_rows++;
    int max_x=std::max(0,visible_count*column_width-std::max(0,int(rect.right)-row_width));
    int max_y=std::max(0,visible_rows*row_height-std::max(0,int(rect.bottom)-header_height));
    scroll_x=std::clamp(scroll_x,0,max_x); scroll_y=std::clamp(scroll_y,0,max_y);
    SCROLLINFO si{sizeof(si),SIF_RANGE|SIF_PAGE|SIF_POS}; si.nMin=0; si.nMax=visible_count*column_width-1; si.nPage=std::max(1,int(rect.right)-row_width); si.nPos=scroll_x; SetScrollInfo(pane,SB_HORZ,&si,TRUE);
    si.nMax=visible_rows*row_height-1; si.nPage=std::max(1,int(rect.bottom)-header_height); si.nPos=scroll_y; SetScrollInfo(pane,SB_VERT,&si,TRUE);
    std::vector<HWND> reusable=std::move(spare_cells);
    for(unsigned r=0;r<input_channels.size();++r) for(unsigned c=0;c<output_channels.size();++c) {
        unsigned index=r*unsigned(output_channels.size())+c;int x=row_width+positions[c]*column_width-scroll_x,y=header_height+rows[r]*row_height-scroll_y;
        bool visible=rows[r]>=0&&positions[c]>=0&&x>=row_width-column_width/2&&x<rect.right&&y>=header_height-row_height/2&&y<rect.bottom;
        if(!visible&&cells[index]) {reusable.push_back(cells[index]);cells[index]=nullptr;}
    }
    SendMessageW(pane,WM_SETREDRAW,FALSE,0);
    HDWP batch=BeginDeferWindowPos(int(cells.size()+row_labels.size()+column_labels.size()+1));
    auto place=[&](HWND control,int x,int y,int w,int h,bool visible,bool header=false) {
        auto found=placements.find(control);
        if(found!=placements.end()) {auto& old=found->second;if(!header&&old.visible==visible&&(!visible||(old.x==x&&old.y==y&&old.w==w&&old.h==h))) return;}
        placements[control]={x,y,w,h,visible};
        UINT flags=SWP_NOACTIVATE|SWP_NOREDRAW|(header?0:SWP_NOZORDER)|(visible?SWP_SHOWWINDOW:SWP_HIDEWINDOW);
        if(batch) batch=DeferWindowPos(batch,control,header?HWND_TOP:nullptr,x,y,w,h,flags);
        else SetWindowPos(control,header?HWND_TOP:nullptr,x,y,w,h,flags);
    };
    for(unsigned r=0;r<input_channels.size();++r) for(unsigned c=0;c<output_channels.size();++c) {
        unsigned index=r*unsigned(output_channels.size())+c;
        int x=row_width+positions[c]*column_width-scroll_x,y=header_height+rows[r]*row_height-scroll_y;
        bool visible=rows[r]>=0 && positions[c]>=0 && x>=row_width-column_width/2 && x<rect.right && y>=header_height-row_height/2 && y<rect.bottom;
        if(!visible) {if(cells[index]) place(cells[index],0,0,40,30,false);continue;}
        if(!cells[index]) {
            auto label=aggregate::wide(input_channels[r].name+" → "+output_channels[c].name);
            if(reusable.empty()) cells[index]=make(pane,L"BUTTON",accessibility?L"":label,BS_AUTOCHECKBOX|WS_TABSTOP,x+45,y+2,40,30,cell_base+index);
            else {cells[index]=reusable.back();reusable.pop_back();SetWindowLongPtrW(cells[index],GWLP_ID,cell_base+index);if(!accessibility) SetWindowTextW(cells[index],label.c_str());}
            if(accessibility) accessibility->SetHwndPropStr(cells[index],OBJID_CLIENT,CHILDID_SELF,PROPID_ACC_NAME,label.c_str());
            SendMessageW(cells[index],BM_SETCHECK,matrix->connected(r,c)?BST_CHECKED:BST_UNCHECKED,0);
            
        }
        place(cells[index],x+45,y+2,40,30,true);
    }
    for(auto control:reusable) place(control,0,0,40,30,false);
    spare_cells=std::move(reusable);
    for(unsigned r=0;r<row_labels.size();++r) {int y=header_height+rows[r]*row_height-scroll_y;place(row_labels[r],0,y,row_width,row_height,rows[r]>=0&&y>=header_height-row_height/2&&y<rect.bottom,true);}
    for(unsigned c=0;c<column_labels.size();++c) {int x=row_width+positions[c]*column_width-scroll_x;place(column_labels[c],x,0,column_width,header_height,positions[c]>=0&&x>=row_width-column_width/2&&x<rect.right,true);}
    place(corner,0,0,row_width,header_height,true,true);
    if(batch) EndDeferWindowPos(batch);
    SendMessageW(pane,WM_SETREDRAW,TRUE,0);
    RedrawWindow(pane,nullptr,nullptr,RDW_ERASE|RDW_FRAME|RDW_INVALIDATE|RDW_ALLCHILDREN);

}
LRESULT CALLBACK pane_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_COMMAND && HIWORD(wp)==BN_CLICKED && LOWORD(wp)>=cell_base && matrix) {
        unsigned index=LOWORD(wp)-cell_base,r=index/matrix->outputs(),c=index%matrix->outputs();
        if(r<matrix->inputs()) matrix->connect(r,c,SendMessageW(reinterpret_cast<HWND>(lp),BM_GETCHECK,0,0)==BST_CHECKED);
        if(resident) try {save_routes();}catch(const std::exception& e) {SetWindowTextW(status_control,aggregate::wide(e.what()).c_str());}
        layout_pane(); return 0;
    }
    if(msg==WM_HSCROLL || msg==WM_VSCROLL || msg==WM_MOUSEWHEEL || msg==WM_MOUSEHWHEEL) {
        bool wheel=msg==WM_MOUSEWHEEL||msg==WM_MOUSEHWHEEL;
        bool vertical=msg==WM_VSCROLL||(msg==WM_MOUSEWHEEL&&!(GET_KEYSTATE_WPARAM(wp)&MK_SHIFT));
        int& position=vertical?scroll_y:scroll_x;
        int step=vertical?row_height:column_width;
        if(wheel) {
            // Preserve sub-pixel remainders from high-resolution wheels independently per axis.
            static int horizontal_remainder=0,vertical_remainder=0;
            int& remainder=vertical?vertical_remainder:horizontal_remainder;
            int delta=GET_WHEEL_DELTA_WPARAM(wp)*(msg==WM_MOUSEHWHEEL?1:-1);
            int movement=remainder+delta*step;
            position+=movement/WHEEL_DELTA;
            remainder=movement%WHEEL_DELTA;
            layout_pane();return 0;
        }
        SCROLLINFO info{sizeof(info),SIF_ALL}; GetScrollInfo(hwnd,vertical?SB_VERT:SB_HORZ,&info);
        switch(LOWORD(wp)) {
            case SB_LINEUP:position-=step;break;
            case SB_LINEDOWN:position+=step;break;
            case SB_PAGEUP:position-=info.nPage;break;
            case SB_PAGEDOWN:position+=info.nPage;break;
            case SB_THUMBTRACK:case SB_THUMBPOSITION:position=info.nTrackPos;break;
        }
        layout_pane(); return 0;
    }
    if(msg==WM_SIZE && pane) layout_pane();
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void resize() {
    RECT rect{}; GetClientRect(window,&rect); int w=rect.right,h=rect.bottom;
    delay_panel.resize(w);
    SetWindowPos(tabs,nullptr,16,158,w-32,32,SWP_NOZORDER);
    SetWindowPos(pane,nullptr,16,230,w-32,std::max(100,h-280),SWP_NOZORDER);
    SetWindowPos(status_control,nullptr,16,h-34,w-32,26,SWP_NOZORDER);
    SetWindowPos(report_control,nullptr,16,300,w-32,std::max(80,h-350),SWP_NOZORDER);
}
void select_tab(int index) {
    selected_tab=index;TabCtrl_SetCurSel(tabs,index);
    ShowWindow(pane,index==0?SW_SHOW:SW_HIDE);ShowWindow(clear_control,index==0?SW_SHOW:SW_HIDE);ShowWindow(all_control,index==0?SW_SHOW:SW_HIDE);
    settings.show(index==1);
    delay_panel.show(index==3);
    for(auto c:{tone_control,report_control,diagnostic_note,simulation_control,silent_control}) ShowWindow(c,index==2?SW_SHOW:SW_HIDE);
    if(index==0) layout_pane();
}
void apply_settings() {
    // Keep the old port layout alive until enumeration and file replacement succeed.
    auto previous=config;auto old_delays=port_delays;auto old_inputs=input_channels,old_outputs=output_channels;auto old_loopbacks=loopback_specs;auto old_matrix=std::move(matrix);auto old_unresolved=unresolved_routes;auto old_note=startup_note;auto old_channels=loopback_channels;
    try {
        config=aggregate::load_config(pending_settings);unresolved_routes.clear();startup_note.clear();inventory();load_routes();
        matrix->gain=old_matrix->gain.load();matrix->muted=old_matrix->muted.load();
        if(!MoveFileExW(pending_settings.c_str(),(root/L"config/aggregate.ini").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot apply settings");
    } catch(...) {
        config=previous;port_delays=std::move(old_delays);input_channels=std::move(old_inputs);output_channels=std::move(old_outputs);loopback_specs=std::move(old_loopbacks);matrix=std::move(old_matrix);unresolved_routes=std::move(old_unresolved);startup_note=old_note;loopback_channels=old_channels;throw;
    }
    rebuild_grid();delay_panel.refresh(input_channels,loopback_channels,output_channels,port_delays,*matrix);SetWindowTextW(status_control,L"設定を適用 / ポート一覧も更新");
    if(resume_after_apply) start();
}
void rebuild_grid() {
    for(auto cell:cells) if(cell) {if(accessibility) accessibility->ClearHwndProps(cell,OBJID_CLIENT,CHILDID_SELF,&PROPID_ACC_NAME,1);DestroyWindow(cell);}
    for(auto cell:spare_cells) {if(accessibility) accessibility->ClearHwndProps(cell,OBJID_CLIENT,CHILDID_SELF,&PROPID_ACC_NAME,1);DestroyWindow(cell);}
    for(auto c:row_labels) DestroyWindow(c);for(auto c:column_labels) DestroyWindow(c);if(corner) DestroyWindow(corner);
    cells.assign(input_channels.size()*output_channels.size(),nullptr);spare_cells.clear();row_labels.clear();column_labels.clear();placements.clear();displayed_text.clear();scroll_x=scroll_y=0;
    corner=make(pane,L"STATIC",L"入力 ↓ ／ 出力 →\n入力レベル dBFS",SS_CENTER,0,0,row_width,header_height);
    for(unsigned c=0;c<output_channels.size();++c) column_labels.push_back(make(pane,L"STATIC",aggregate::wide(output_channels[c].name),SS_CENTER,row_width+c*column_width,0,column_width,header_height));
    for(unsigned r=0;r<input_channels.size();++r) row_labels.push_back(make(pane,L"STATIC",aggregate::wide(input_channels[r].name),SS_LEFT,0,header_height+r*row_height,row_width,row_height));
    layout_pane();
}
aggregate::Config routed_config() {
    auto active=config;
    active.output_delay_frames=aggregate::delay_frames(port_delays,output_channels,config.sample_rate);
    for(auto& spec:active.playback) {
        spec.enabled=false;
        for(unsigned c=0;c<output_channels.size();++c) if(output_channels[c].endpoint==spec.id&&matrix->output_connected(c)) spec.enabled=true;
    }
    return active;
}
std::vector<bool> active_outputs(const aggregate::Config& active) {std::vector<bool> result;for(auto& spec:active.playback) result.push_back(spec.enabled);return result;}
std::string calibration_label(std::string text) {
    for(auto c:calibration_outputs) {auto marker="出力ch "+std::to_string(c+1)+" /";auto at=text.find(marker);
        if(at!=std::string::npos) text.replace(at,marker.size(),output_channels[c].name+" /");}
    return text;
}
void start() {
    if(busy) return;
    if(worker.joinable()) worker.join();
    stop_event=CreateEventW(nullptr,TRUE,FALSE,nullptr); if(!stop_event) throw std::runtime_error("Cannot create stop event");
    busy=true; EnableWindow(tone_control,FALSE); EnableWindow(simulation_control,FALSE);EnableWindow(silent_control,FALSE);EnableWindow(start_control,FALSE); EnableWindow(stop_control,TRUE);
    EnableWindow(mute_control,!diagnostic_running&&!simulation_running&&!calibration_running);
    if(calibration_running) {settings.enable(false);delay_panel.enable(false);EnableWindow(gain_control,FALSE);}
    SetWindowTextW(status_control,L"機器を初期化中… ミュート状態を確認");
    worker=std::thread([] {
        auto final=std::make_unique<Update>(); final->final=true;
        try {
            if(calibration_running) {
                std::ofstream trace(root/L"build/results/calibration-last.txt");
                trace<<"microphone="<<input_channels[loopback_channels+calibration_input].name<<" input_index="<<calibration_input<<"\n";
                for(auto c:calibration_outputs) trace<<"output_index="<<c<<" "<<output_channels[c].name<<"\n";
                auto active=config;active.output_delay_ms.clear();active.output_delay_frames.clear();
                for(auto& spec:active.playback) {spec.enabled=false;for(auto c:calibration_outputs) if(output_channels[c].endpoint==spec.id) spec.enabled=true;}
                unsigned raw=0;for(unsigned i=loopback_channels;i<input_channels.size();++i) if(input_channels[i].key.rfind("asio:",0)==0) ++raw;
                for(auto& spec:active.capture) {spec.enabled=calibration_input>=raw&&calibration_input<raw+spec.channels;raw+=spec.channels;}
                final->calibration=aggregate::calibrate_outputs(active,calibration_input,calibration_outputs,stop_event,[&](const std::string& text) {auto display=calibration_label(text);trace<<display<<"\n";trace.flush();auto update=std::make_unique<Update>();update->text=display;PostMessageW(window,update_message,0,reinterpret_cast<LPARAM>(update.release()));},calibration_gain);
                trace<<"success\n";final->calibrated=true;final->text="自動計測完了 / 補正後の出力差は2 ms以内";
                PostMessageW(window,update_message,0,reinterpret_cast<LPARAM>(final.release()));return;
            }
            if(simulation_running) {
                aggregate::Config simulated;simulated.simulation=true;simulated.capture={{"sim-0",2}};simulated.playback={{"sim-0",2}};
                aggregate::Engine engine(simulated);
                engine.prepare([](void*,const float* input,float* output,unsigned frames){std::copy_n(input,size_t(frames)*2,output);},nullptr);
                engine.start();WaitForSingleObject(stop_event,2000);engine.stop();final->text="人工信号テスト（実機を使用しない）\n"+engine.diagnostics();final->health=engine.health_flags();
                PostMessageW(window,update_message,0,reinterpret_cast<LPARAM>(final.release()));return;
            }
            auto fingerprint=aggregate::device_fingerprint;
            auto began=GetTickCount64();
            while(WaitForSingleObject(stop_event,0)==WAIT_TIMEOUT) {
                try {
                    auto before=fingerprint();
                    auto active=routed_config();auto enabled=active_outputs(active);
                    if(diagnostic_running) {for(auto& endpoint:active.playback) endpoint.enabled=false;}
                    aggregate::RouterSession session(active,loopback_specs,*matrix); session.start();
                    unsigned poll=0;
                    while(WaitForSingleObject(stop_event,250)==WAIT_TIMEOUT) {
                        auto update=std::make_unique<Update>(); update->text=session.diagnostics();update->health=session.health_flags();PostMessageW(window,update_message,0,reinterpret_cast<LPARAM>(update.release()));
                        if(!diagnostic_running&&active_outputs(routed_config())!=enabled) break;
                        if(diagnostic_running&&GetTickCount64()-began>=600000) {SetEvent(stop_event);break;}
                        if(++poll%8==0 && (session.recovery_requested() || fingerprint()!=before)) break;
                    }
                    session.stop();final->text=session.diagnostics();final->health=session.health_flags();final->error.clear();
                } catch(const std::exception& e) {
                    final->error=e.what();auto update=std::make_unique<Update>();update->text="再接続待ち: "+final->error;update->health=2;PostMessageW(window,update_message,0,reinterpret_cast<LPARAM>(update.release()));
                }
                if(WaitForSingleObject(stop_event,1000)!=WAIT_TIMEOUT) break;
            }
        } catch(const std::exception& e) {final->error=e.what();}
        std::error_code ignored; std::filesystem::create_directories(root/L"build/results",ignored);
        { std::ofstream out(root/L"build/results/router-last.txt"); out<<final->text<<"\nerror="<<final->error<<"\nhealth_flags="<<final->health<<"\nblocks="<<matrix->blocks.load()<<"\n";
          for(unsigned i=0;i<input_channels.size();++i) out<<"input_peak "<<input_channels[i].key<<' '<<matrix->input_peak(i)<<"\n";
          for(unsigned o=0;o<output_channels.size();++o) out<<"output_peak "<<output_channels[o].key<<' '<<matrix->output_peak(o)<<"\n"; }
        PostMessageW(window,update_message,0,reinterpret_cast<LPARAM>(final.release()));
    });
}
LRESULT CALLBACK procedure(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==tray_message) {
        if(lp==WM_LBUTTONUP || lp==WM_LBUTTONDBLCLK) show_router();
        if(lp==WM_RBUTTONUP) {POINT point{};GetCursorPos(&point);auto menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,tray_open_id,L"ルーティングを開く");AppendMenuW(menu,MF_STRING,tray_exit_id,L"終了（音声を停止）");SetForegroundWindow(hwnd);TrackPopupMenu(menu,TPM_RIGHTBUTTON,point.x,point.y,0,hwnd,nullptr);DestroyMenu(menu);PostMessageW(hwnd,WM_NULL,0,0);}
        return 0;
    }
    if(msg==RegisterWindowMessageW(L"TaskbarCreated")) {Shell_NotifyIconW(NIM_ADD,&tray);return 0;}
    if(msg==WM_GETMINMAXINFO) {auto* info=reinterpret_cast<MINMAXINFO*>(lp); info->ptMinTrackSize={960,780}; return 0;}
    if(msg==WM_NOTIFY && reinterpret_cast<NMHDR*>(lp)->hwndFrom==tabs && reinterpret_cast<NMHDR*>(lp)->code==TCN_SELCHANGE) {select_tab(TabCtrl_GetCurSel(tabs));return 0;}
    if(msg==WM_NOTIFY) {try {delay_panel.notify(reinterpret_cast<NMHDR*>(lp));}catch(const std::exception& e) {SetWindowTextW(status_control,aggregate::wide(e.what()).c_str());}}
    if(msg==WM_SIZE && pane) resize();
    if(msg==WM_HSCROLL && reinterpret_cast<HWND>(lp)==gain_control && matrix) {
        matrix->gain=float(SendMessageW(gain_control,TBM_GETPOS,0,0))/100;
        if(resident && LOWORD(wp)==TB_ENDTRACK) try {save_routes();}catch(const std::exception& e) {SetWindowTextW(status_control,aggregate::wide(e.what()).c_str());}
    }
    if(msg==WM_COMMAND && matrix) try {
        delay_panel.command(LOWORD(wp),HIWORD(wp));
        switch(LOWORD(wp)) {
            case aggregate::DelayPanel::apply_id: {
                if(busy||applying) throw std::runtime_error("共通の停止ボタンで停止してから遅延を適用してください");
                auto previous=port_delays;port_delays=delay_panel.candidate();try {save_routes();}catch(...) {port_delays=std::move(previous);throw;}
                SetWindowTextW(status_control,L"出力名ごとの遅延を保存 / 次の音声開始から反映");break;
            }
            case aggregate::DelayPanel::calibrate_id:
                if(applying||diagnostic_running||simulation_running||calibration_running||calibration_pending) throw std::runtime_error("現在の処理を停止してから計測してください");
                calibration_input=delay_panel.microphone();calibration_gain=delay_panel.test_gain();calibration_outputs=delay_panel.selected_outputs();resume_after_calibration=busy;
                delay_panel.progress("通常転送を解放して計測を準備中…");
                if(busy) {calibration_pending=true;settings.enable(false);delay_panel.enable(false);SetEvent(stop_event);}
                else {calibration_running=true;start();}break;
            case 600:
            case 601:
                if(busy||applying) throw std::runtime_error("共通の停止ボタンで停止してからテストしてください");
                diagnostic_running=LOWORD(wp)==600;simulation_running=LOWORD(wp)==601;
                diagnostic_previous_mute=matrix->muted.load();matrix->muted=true;SendMessageW(mute_control,BM_SETCHECK,BST_CHECKED,0);resident_running=false;start();break;
            case aggregate::SettingsPanel::apply_id:
            case aggregate::SettingsPanel::refresh_id:
                if(applying) break;
                if(diagnostic_running||simulation_running||calibration_running) throw std::runtime_error("テスト停止後に設定を適用してください");
                if(LOWORD(wp)==aggregate::SettingsPanel::refresh_id) {pending_settings=root/L"build/settings-candidate.ini";if(!CopyFileW((root/L"config/aggregate.ini").c_str(),pending_settings.c_str(),FALSE)) throw std::runtime_error("Cannot read saved settings");}
                else pending_settings=settings.candidate(root/L"build/settings-candidate.ini");
                save_routes();resume_after_apply=busy;settings.enable(false);applying=true;
                if(busy) {SetEvent(stop_event);SetWindowTextW(status_control,L"設定適用のため機器を解放中…");}
                else {try {apply_settings();settings.refresh(aggregate::load_config(root/L"config/aggregate.ini"));}catch(...) {applying=false;settings.enable(true);throw;}applying=false;settings.enable(true);}
                break;
            case aggregate::SettingsPanel::panel_id:
                if(busy) throw std::runtime_error("メーカー設定を開く前に共通の停止ボタンで停止してください");
                settings.control_panel();break;
            
            case all_outputs_id:all_outputs=SendMessageW(reinterpret_cast<HWND>(lp),BM_GETCHECK,0,0)==BST_CHECKED;scroll_x=0;scroll_y=0;layout_pane();break;
            case mute_id:matrix->muted=SendMessageW(mute_control,BM_GETCHECK,0,0)==BST_CHECKED;break;
            case tone_id:matrix->test_tone=SendMessageW(tone_control,BM_GETCHECK,0,0)==BST_CHECKED;layout_pane();break;
            case tray_open_id:show_router();break;
            case tray_exit_id:if(resident) {save_routes();save_resident();}request_exit();break;
            case resident_id: {
                bool enabled=SendMessageW(resident_control,BM_GETCHECK,0,0)==BST_CHECKED;
                try {register_startup(enabled);resident=enabled;resident_running=busy&&!diagnostic_running&&!simulation_running;save_routes();save_resident();}
                catch(...) {SendMessageW(resident_control,BM_SETCHECK,resident?BST_CHECKED:BST_UNCHECKED,0);throw;}
                break;
            }
            case save_id:save_routes();if(resident) save_resident();SetWindowTextW(status_control,resident?L"接続と常用状態を保存":L"接続を保存 / 次回起動時もミュートON");break;
            case start_id:if(applying) break;diagnostic_running=false;simulation_running=false;start();resident_running=true;if(resident) {save_routes();save_resident();}break;
            case stop_id:calibration_pending=false;resume_after_calibration=false;if(applying) resume_after_apply=false;resident_running=false;if(resident) save_resident();SetEvent(stop_event);EnableWindow(stop_control,FALSE);SetWindowTextW(status_control,L"音声を停止し、機器を解放中…");break;
            case clear_id:unresolved_routes.clear();for(unsigned r=0;r<matrix->inputs();++r) for(unsigned c=0;c<matrix->outputs();++c) matrix->connect(r,c,false);
                for(auto cell:cells) if(cell) SendMessageW(cell,BM_SETCHECK,BST_UNCHECKED,0);layout_pane();break;
        }
        if(resident && !calibration_running && (LOWORD(wp)==mute_id || LOWORD(wp)==clear_id)) {save_routes();save_resident();}
    } catch(const std::exception& e) {if(!busy) calibration_running=false;if(LOWORD(wp)==aggregate::DelayPanel::calibrate_id) delay_panel.progress(e.what());SetWindowTextW(status_control,aggregate::wide(std::string("エラー: ")+e.what()).c_str());}
    if(msg==update_message) {
        std::unique_ptr<Update> update(reinterpret_cast<Update*>(lp));
        if(calibration_running) {
            update->error=calibration_label(update->error);
            delay_panel.progress(update->error.empty()?update->text:update->error);
            if(update->final&&update->calibrated) try {
                auto previous=port_delays;auto next=port_delays;
                for(unsigned i=0;i<calibration_outputs.size();++i) next[output_channels[calibration_outputs[i]].key]=update->calibration.corrections[i]*1000./config.sample_rate;
                port_delays=std::move(next);try {save_routes();}catch(...) {port_delays=std::move(previous);throw;}
                delay_panel.results(calibration_outputs,update->calibration.measured,update->calibration.corrections,config.sample_rate);
                update->text+=" / 保存済み、次の音声開始から反映";delay_panel.progress(update->text);
            }catch(const std::exception& e) {update->error=e.what();delay_panel.progress(update->error);}
        }
        if(update->final&&(diagnostic_running||simulation_running)) {matrix->muted=diagnostic_previous_mute;SendMessageW(mute_control,BM_SETCHECK,diagnostic_previous_mute?BST_CHECKED:BST_UNCHECKED,0);if(resident) try {save_resident();}catch(const std::exception& e) {update->error=e.what();}}
        auto text=aggregate::wide(update->text); SetWindowTextW(report_control,text.c_str());
        std::wstring state=update->final?L"停止済み / ":update->text.rfind("再接続待ち:",0)==0?L"再接続待ち / ":diagnostic_running?L"無音テスト中 / ":L"音声転送中 / ";
        state+=matrix->muted.load()?L"ミュートON / ":L"ミュートOFF / ";
        state+=L"処理 "+std::to_wstring(matrix->blocks.load())+L" ブロック / "+(update->health?L"バッファ等の異常あり":L"検出異常なし");
        if(update->final&&simulation_running) state=update->health?L"人工信号テスト終了 / 異常あり（診断結果を確認）":L"人工信号テスト終了 / 検出異常なし";
        if(calibration_running) state=aggregate::wide(update->text);
        if(!update->error.empty()) state=L"エラー: "+aggregate::wide(update->error);
        SetWindowTextW(status_control,state.c_str());
        if(update->final) {
            bool was_calibration=calibration_running;
            if(was_calibration&&!update->error.empty()) {std::ofstream trace(root/L"build/results/calibration-last.txt",std::ios::app);trace<<"error="<<update->error<<"\n";}
            worker.join();CloseHandle(stop_event);stop_event=nullptr;busy=false;diagnostic_running=false;simulation_running=false;calibration_running=false;settings.enable(true);delay_panel.enable(true);EnableWindow(gain_control,TRUE);EnableWindow(mute_control,TRUE);EnableWindow(tone_control,TRUE);EnableWindow(simulation_control,TRUE);EnableWindow(silent_control,TRUE);EnableWindow(start_control,TRUE);EnableWindow(stop_control,FALSE);
            if(closing) DestroyWindow(hwnd);
            else if(calibration_pending) {calibration_pending=false;calibration_running=true;start();}
            else if(was_calibration&&resume_after_calibration) {resume_after_calibration=false;start();}
            else if(applying) {try {apply_settings();settings.refresh(aggregate::load_config(root/L"config/aggregate.ini"));}catch(const std::exception& e) {SetWindowTextW(status_control,aggregate::wide(std::string("設定を適用できません: ")+e.what()).c_str());if(resume_after_apply) start();}applying=false;settings.enable(true);}
        }
    }
    if(msg==WM_TIMER && matrix) {
        if(!closing&&!applying&&!calibration_running&&!calibration_pending&&!diagnostic_running&&!simulation_running&&device_watcher.changed.exchange(false)) {
            try {SendMessageW(hwnd,WM_COMMAND,aggregate::SettingsPanel::refresh_id,0);}
            catch(const std::exception& e) {SetWindowTextW(status_control,aggregate::wide(e.what()).c_str());}
        }

        matrix->gain=float(SendMessageW(gain_control,TBM_GETPOS,0,0))/100;
        matrix->muted=SendMessageW(mute_control,BM_GETCHECK,0,0)==BST_CHECKED;
        for(unsigned r=0;r<row_labels.size();++r) if(IsWindowVisible(row_labels[r])) changed_text(row_labels[r],(aggregate::wide(input_channels[r].name)+L"  "+db(busy?matrix->input_peak(r):0)+L" dB").c_str());
        for(unsigned c=0;c<column_labels.size();++c) {
            const auto& channel=output_channels[c];
            std::string label=channel.name;
            auto slash=label.find('/'); if(slash!=std::string::npos) label.replace(slash,1,"\n");
            if(IsWindowVisible(column_labels[c])) changed_text(column_labels[c],(aggregate::wide(label)+L"\n"+db(busy?matrix->output_peak(c):0)+L" dB").c_str());
        }
        changed_text(gain_label,(L"出力音量 "+std::to_wstring(int(std::round(matrix->gain.load()*100)))+L"% / 超過 "+std::to_wstring(matrix->clipped_samples.load())).c_str());
    }
    if(msg==WM_CLOSE) {if(resident) {try {save_routes();save_resident();ShowWindow(hwnd,SW_HIDE);}catch(const std::exception& e) {SetWindowTextW(status_control,aggregate::wide(e.what()).c_str());}}else request_exit();return 0;}
    if(msg==WM_QUERYENDSESSION) return TRUE;
    if(msg==WM_ENDSESSION && wp) {if(busy) {SetEvent(stop_event);if(worker.joinable()) worker.join();busy=false;}return 0;}
    if(msg==WM_DESTROY) {device_watcher.stop();Shell_NotifyIconW(NIM_DELETE,&tray);KillTimer(hwnd,1);for(auto cell:cells) if(cell&&accessibility) accessibility->ClearHwndProps(cell,OBJID_CLIENT,CHILDID_SELF,&PROPID_ACC_NAME,1);PostQuitMessage(0);return 0;}
    return DefWindowProcW(hwnd,msg,wp,lp);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR command,int show) {
    SetProcessDPIAware(); InitCommonControls();
    wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);root=std::filesystem::path(executable).parent_path().parent_path();
    HANDLE single=CreateMutexW(nullptr,FALSE,L"Local\\AudioAggregateRoutingWindow");
    if(GetLastError()==ERROR_ALREADY_EXISTS) {if(auto existing=FindWindowW(L"AudioAggregateRouter",nullptr)) {ShowWindow(existing,SW_RESTORE);SetForegroundWindow(existing);}if(single) CloseHandle(single);return 0;}
    try {
        auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        bool com_ready=SUCCEEDED(com);
        if(com_ready) CoCreateInstance(CLSID_AccPropServices,nullptr,CLSCTX_INPROC_SERVER,IID_IAccPropServices,reinterpret_cast<void**>(&accessibility));
        config=aggregate::load_config(root/L"config/aggregate.ini");inventory();
        resident_test=std::wstring(command).find(L"--resident-self-test")!=std::wstring::npos;
        {std::ifstream saved(root/L"config/resident.txt");saved>>resident>>resident_running>>resident_muted;}
        font=CreateFontW(-15,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Yu Gothic UI");
        WNDCLASSW wc{};wc.lpfnWndProc=procedure;wc.hInstance=instance;wc.lpszClassName=L"AudioAggregateRouter";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);RegisterClassW(&wc);
        wc.lpfnWndProc=pane_proc;wc.lpszClassName=L"AudioAggregateRouteGrid";wc.hbrBackground=GetSysColorBrush(COLOR_WINDOW);RegisterClassW(&wc);
        RECT area{};SystemParametersInfoW(SPI_GETWORKAREA,0,&area,0);
        window=CreateWindowW(L"AudioAggregateRouter",L"AASAPI",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,std::min(1320L,area.right-area.left-40),std::min(920L,area.bottom-area.top-40),nullptr,nullptr,instance,nullptr);
        make(window,L"STATIC",L"AASAPI / 主ASIOのクロックで複数機器を接続",0,16,12,1100,24);
        resident_control=make(window,L"BUTTON",L"常用：ログイン時に復元／×でトレイへ",BS_AUTOCHECKBOX|WS_TABSTOP,16,43,440,26,resident_id);SendMessageW(resident_control,BM_SETCHECK,resident?BST_CHECKED:BST_UNCHECKED,0);
        mute_control=make(window,L"BUTTON",L"ミュート",BS_AUTOCHECKBOX|WS_TABSTOP,16,80,150,26,mute_id);SendMessageW(mute_control,BM_SETCHECK,resident&&!resident_muted?BST_UNCHECKED:BST_CHECKED,0);
        gain_control=make(window,TRACKBAR_CLASSW,L"出力音量",TBS_HORZ|WS_TABSTOP,180,78,250,32,gain_id);SendMessageW(gain_control,TBM_SETRANGE,TRUE,MAKELONG(0,100));
        gain_label=make(window,L"STATIC",L"出力音量",0,450,82,400,26);
        start_control=make(window,L"BUTTON",L"音声を開始",WS_TABSTOP,16,120,140,28,start_id);
        stop_control=make(window,L"BUTTON",L"停止",WS_TABSTOP,170,120,95,28,stop_id);EnableWindow(stop_control,FALSE);
        make(window,L"BUTTON",L"接続を保存",WS_TABSTOP,280,120,140,28,save_id);
        make(window,L"BUTTON",L"完全終了",WS_TABSTOP,435,120,140,28,tray_exit_id);
        tabs=make(window,WC_TABCONTROLW,L"画面切替",WS_TABSTOP,16,158,1250,32);
        for(auto label:{L"ルーティング",L"機器設定",L"診断",L"遅延補正"}) {TCITEMW tab{};tab.mask=TCIF_TEXT;tab.pszText=const_cast<wchar_t*>(label);TabCtrl_InsertItem(tabs,TabCtrl_GetItemCount(tabs),&tab);}
        clear_control=make(window,L"BUTTON",L"接続を全解除",WS_TABSTOP,16,195,140,28,clear_id);
        all_control=make(window,L"BUTTON",L"認識した全入出力を表示",BS_AUTOCHECKBOX|WS_TABSTOP,180,195,300,28,all_outputs_id);SendMessageW(all_control,BM_SETCHECK,BST_CHECKED,0);
        tone_control=make(window,L"BUTTON",L"再生音行を確認音440 Hzに置換（−30 dBFS）",BS_AUTOCHECKBOX|WS_TABSTOP,16,202,650,26,tone_id);
        diagnostic_note=make(window,L"STATIC",L"診断は停止後に実行 / 無音テストは最大600秒 / 処理時間は実測の往復遅延ではない",0,16,240,1150,24);
        silent_control=make(window,L"BUTTON",L"無音で実機をテスト",WS_TABSTOP,16,270,230,28,600);
        simulation_control=make(window,L"BUTTON",L"人工信号テスト（2秒）",WS_TABSTOP,270,270,260,28,601);
        status_control=make(window,L"STATIC",L"ミュートON / チェック → 音声開始 → 音量確認 → ミュート解除",0,16,720,1250,26);
        report_control=make(window,L"EDIT",L"",WS_BORDER|ES_READONLY|ES_MULTILINE|WS_VSCROLL|WS_HSCROLL|ES_AUTOHSCROLL|WS_TABSTOP,16,752,1250,60);
        settings.create(window,font,aggregate::load_config(root/L"config/aggregate.ini"));
        delay_panel.create(window,font);
        cells.resize(input_channels.size()*output_channels.size());
        load_routes();SendMessageW(gain_control,TBM_SETPOS,TRUE,int(std::round(matrix->gain.load()*100)));
        delay_panel.refresh(input_channels,loopback_channels,output_channels,port_delays,*matrix);{std::ifstream saved(root/L"config/router.txt");delay_panel.load_selection(saved);}
        matrix->muted=!(resident&&!resident_muted);
        tray.cbSize=sizeof(tray);tray.hWnd=window;tray.uID=1;tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;tray.uCallbackMessage=tray_message;tray.hIcon=LoadIconW(nullptr,IDI_APPLICATION);wcscpy_s(tray.szTip,L"AASAPI — ルーティング");
        if(!Shell_NotifyIconW(NIM_ADD,&tray)) throw std::runtime_error("Cannot create notification area icon");
        pane=make(window,L"AudioAggregateRouteGrid",L"入力と出力の接続",WS_BORDER|WS_VSCROLL|WS_HSCROLL|WS_CLIPCHILDREN,16,200,1250,500);
        rebuild_grid();resize();select_tab(0);
        if(std::wstring(command).find(L"--calibration-cancel-self-test")!=std::wstring::npos) {
            auto active=config;for(auto& spec:active.playback) spec.enabled=false;for(auto& spec:active.capture) spec.enabled=false;
            HANDLE cancel=CreateEventW(nullptr,TRUE,TRUE,nullptr);bool canceled=false;
            try {aggregate::calibrate_outputs(active,0,{0,1},cancel,[](auto&){});}catch(const std::exception& e) {canceled=std::string(e.what()).find("中止")!=std::string::npos;}CloseHandle(cancel);
            if(!canceled) throw std::runtime_error("Real-device calibration cancellation failed");
            std::ofstream result(root/L"build/results/calibration-cancel.txt");result<<"PASS: real native ASIO calibration engine canceled before test signal; configuration unchanged\n";result.close();DestroyWindow(window);if(accessibility) accessibility->Release();if(com_ready) CoUninitialize();if(single) CloseHandle(single);return 0;
        }
        if(std::wstring(command).find(L"--ui-self-test")!=std::wstring::npos) {
            ShowWindow(window,SW_SHOWNOACTIVATE);
            settings.self_test(root/L"build/ui-test-candidate.ini");
            delay_panel.self_test();
            for(int tab:{1,2,3,0,3,2,1,0}) {select_tab(tab);rebuild_grid();resize();if((IsWindowVisible(pane)!=FALSE)!=(tab==0) || (IsWindowVisible(report_control)!=FALSE)!=(tab==2)) throw std::runtime_error("Tab visibility mismatch after rebuilding ports");}
            scroll_x=scroll_y=0;layout_pane();
            SendMessageW(pane,WM_MOUSEWHEEL,MAKEWPARAM(MK_SHIFT,WORD(-WHEEL_DELTA)),0);
            if(scroll_x!=column_width||scroll_y!=0) throw std::runtime_error("Shift wheel did not scroll horizontally");
            SendMessageW(pane,WM_MOUSEHWHEEL,MAKEWPARAM(0,WORD(-WHEEL_DELTA)),0);
            if(scroll_x!=0||scroll_y!=0) throw std::runtime_error("Horizontal wheel direction mismatch");
            SendMessageW(pane,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(-WHEEL_DELTA)),0);
            if(scroll_y!=row_height||scroll_x!=0) throw std::runtime_error("Normal wheel did not scroll vertically");
            for(int i=0;i<120;++i) SendMessageW(pane,WM_MOUSEWHEEL,MAKEWPARAM(MK_SHIFT,WORD(-1)),0);
            if(scroll_x!=column_width||scroll_y!=row_height) throw std::runtime_error("High-resolution wheel delta lost");
            std::ofstream result(root/L"build/results/ui-self-test.txt");result<<"PASS: four tabs, named delay editing and stereo link, invalid delay rejection, shared transport controls, invalid sample rate rejected, saved settings round-trip; normal/Shift/horizontal wheel direction and 120 fractional deltas; no audio or saved configuration changes\n";result.close();DestroyWindow(window);if(accessibility) accessibility->Release();if(com_ready) CoUninitialize();if(single) CloseHandle(single);return 0;
        }
        if(std::wstring(command).find(L"--layout-benchmark")!=std::wstring::npos) {
            std::ofstream result(root/L"build/results/layout-benchmark.txt");LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);
            for(auto size:std::vector<std::pair<int,int>>{{1280,800},{1920,1080},{3840,2160}}) {
                SetWindowPos(window,nullptr,0,0,size.first,size.second,SWP_NOZORDER|SWP_NOACTIVATE);
                std::vector<double> times;
                for(unsigned pass=0;pass<2;++pass) {times.clear();for(int i=0;i<80;++i) {scroll_x=(i%9)*column_width*3;scroll_y=(i%7)*row_height*6;LARGE_INTEGER before,after;QueryPerformanceCounter(&before);layout_pane();QueryPerformanceCounter(&after);
                    for(unsigned cell_index=0;cell_index<cells.size();++cell_index) if(cells[cell_index]) {
                        if(GetDlgCtrlID(cells[cell_index])!=int(cell_base+cell_index) || (SendMessageW(cells[cell_index],BM_GETCHECK,0,0)==BST_CHECKED)!=matrix->connected(cell_index/matrix->outputs(),cell_index%matrix->outputs())) throw std::runtime_error("Reused routing cell identity/state mismatch");
                    }
                    times.push_back((after.QuadPart-before.QuadPart)*1000./frequency.QuadPart);}
                std::sort(times.begin(),times.end());unsigned controls=unsigned(spare_cells.size());for(auto cell:cells) if(cell) ++controls;
                result<<size.first<<"x"<<size.second<<" pass="<<pass<<" median_ms="<<times[40]<<" p95_ms="<<times[76]<<" max_ms="<<times.back()<<" controls="<<controls<<"\n";}
            }
            result<<"PASS: reused cell IDs and check states match routing after 480 layouts; saved routes unchanged\n";result.close();DestroyWindow(window);if(accessibility) accessibility->Release();if(com_ready) CoUninitialize();if(single) CloseHandle(single);return 0;
        }
        device_watcher.start();SetTimer(window,1,200,nullptr);SetWindowTextW(status_control,aggregate::wide(std::string("停止済み / ")+(matrix->muted.load()?"ミュートON":"ミュートOFF")+startup_note).c_str());
        if(resident_test) {
            resident=true;resident_running=true;matrix->muted=true;SendMessageW(mute_control,BM_SETCHECK,BST_CHECKED,0);start();
            SendMessageW(window,WM_CLOSE,0,0);
            bool passed=busy&&!IsWindowVisible(window)&&WaitForSingleObject(stop_event,0)==WAIT_TIMEOUT;
            std::ofstream result(root/L"build/results/resident-self-test.txt");result<<(passed?"PASS":"FAIL")<<": close hides window without stopping worker; output muted\n";
            if(!passed) throw std::runtime_error("Resident lifecycle self-test failed");
            request_exit();
        } else {
            bool background=std::wstring(command).find(L"--background")!=std::wstring::npos;
            if(!(resident&&background)) {ShowWindow(window,show);UpdateWindow(window);}
            if(resident&&resident_running) start();
        }
        MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0) {if(!IsDialogMessageW(window,&msg)) {TranslateMessage(&msg);DispatchMessageW(&msg);}}
        DeleteObject(font);if(accessibility) accessibility->Release();if(com_ready) CoUninitialize();if(single) CloseHandle(single);return 0;
    } catch(const std::exception& e) {
        // Startup has no usable main window yet; a native error box is appropriate here.
        MessageBoxW(nullptr,aggregate::wide(std::string("ルーティング画面を開けません: ")+e.what()).c_str(),L"AASAPI",MB_OK|MB_ICONERROR);
        if(single) CloseHandle(single);return 1;
    }
}
