#pragma once
#include "port_inventory.h"
#include "delay_settings.h"
#include <commctrl.h>
namespace aggregate {
class DelayPanel {
    std::vector<HWND> controls_;
    HWND list_{},mic_{},edit_{},link_{},note_{},level_{};
    std::vector<RouteChannel> outputs_,inputs_;
    unsigned first_input_=0;
    int selected_=-1;
    bool refreshing_=false;
    PortDelays values_;
    static std::wstring text(HWND c) {int n=GetWindowTextLengthW(c);std::wstring s(n+1,L'\0');GetWindowTextW(c,s.data(),n+1);s.resize(n);return s;}
    static std::wstring label(double value) {wchar_t buffer[40]{};swprintf_s(buffer,L"%.3f",value);return buffer;}
    void update_row(unsigned row) {auto value=label(values_[outputs_[row].key]);ListView_SetItemText(list_,row,1,value.data());}
public:
    enum {apply_id=650,calibrate_id,mic_id,edit_id,minus_id,plus_id,link_id,level_id};
    void create(HWND parent,HFONT font) {
        auto add=[&](const wchar_t* kind,const wchar_t* label,DWORD style,int x,int y,int w,int h,int id=0) {
            HWND c=CreateWindowExW(0,kind,label,WS_CHILD|style,x,y,w,h,parent,reinterpret_cast<HMENU>(INT_PTR(id)),nullptr,nullptr);
            if(!c) throw std::runtime_error("Cannot create delay panel");SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);controls_.push_back(c);return c;};
        add(L"STATIC",L"計測マイク（実入力のみ） / 聴く位置に固定し、対象スピーカーを順番に測定",0,20,202,900,24);
        mic_=add(L"COMBOBOX",L"計測マイク",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,20,232,880,300,mic_id);
        add(L"STATIC",L"チェックした出力を同期 / 行を選ぶと手動調整 / 単位 ms",0,20,270,900,24);
        list_=add(WC_LISTVIEWW,L"出力ごとの遅延補正",WS_BORDER|LVS_REPORT|LVS_SINGLESEL|WS_TABSTOP,20,300,880,240);
        ListView_SetExtendedListViewStyle(list_,LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        unsigned col=0;for(auto pair:std::vector<std::pair<const wchar_t*,int>>{{L"出力名 / ch",510},{L"追加遅延 ms",140},{L"計測結果",200}}) {LVCOLUMNW c{};c.mask=LVCF_TEXT|LVCF_WIDTH;c.pszText=const_cast<wchar_t*>(pair.first);c.cx=pair.second;ListView_InsertColumn(list_,col++,&c);}
        add(L"STATIC",L"選択行の追加遅延",0,20,554,180,24);
        edit_=add(L"EDIT",L"0",WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,200,550,120,28,edit_id);
        add(L"BUTTON",L"−0.1",WS_TABSTOP,330,550,80,28,minus_id);add(L"BUTTON",L"＋0.1",WS_TABSTOP,420,550,80,28,plus_id);
        link_=add(L"BUTTON",L"左右を連動",BS_AUTOCHECKBOX|WS_TABSTOP,530,550,200,28,link_id);SendMessageW(link_,BM_SETCHECK,BST_CHECKED,0);
        add(L"BUTTON",L"手動値を適用",WS_TABSTOP,20,590,170,32,apply_id);
        add(L"BUTTON",L"試験音で自動計測・適用",WS_TABSTOP,210,590,300,32,calibrate_id);
        add(L"STATIC",L"試験音量 dBFS",0,530,595,150,24);
        level_=add(L"COMBOBOX",L"試験音量",CBS_DROPDOWNLIST|WS_TABSTOP,680,590,150,160,level_id);
        for(auto label:{L"−36",L"−30",L"−24",L"−18",L"−12",L"−6"}) SendMessageW(level_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));SendMessageW(level_,CB_SETCURSEL,2,0);
        note_=add(L"STATIC",L"同期準備60秒以上 / 予備音＋3回測定 / 残差を追加補正（最大5回・目標0.1ms以内） / 試験音500ms / 停止で中止",0,20,634,900,65);
    }
    void show(bool value) {for(auto c:controls_) ShowWindow(c,value?SW_SHOW:SW_HIDE);}
    void resize(int width) {if(!list_) return;int w=std::max(880,width-40);SetWindowPos(list_,nullptr,20,300,w,240,SWP_NOZORDER);SetWindowPos(mic_,nullptr,20,232,w,300,SWP_NOZORDER);ListView_SetColumnWidth(list_,0,w-370);ListView_SetColumnWidth(list_,2,230);}
    void enable(bool value) {for(auto c:controls_) EnableWindow(c,value);EnableWindow(edit_,value&&selected_>=0);}
    void refresh(const std::vector<RouteChannel>& inputs,unsigned loopbacks,const std::vector<RouteChannel>& outputs,const PortDelays& values,const RoutingMatrix& matrix) {
        std::vector<std::string> checked;for(unsigned i=0;i<outputs_.size();++i) if(ListView_GetCheckState(list_,i)) checked.push_back(outputs_[i].key);
        bool previous=!outputs_.empty();refreshing_=true;auto selected_mic=mic_key();first_input_=loopbacks;inputs_=inputs;outputs_=outputs;values_=values;selected_=-1;
        SendMessageW(mic_,CB_RESETCONTENT,0,0);SendMessageW(mic_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"計測マイクを選択してください"));unsigned mic_selection=0;
        for(unsigned i=loopbacks;i<inputs.size();++i) {auto label=wide(inputs[i].name);SendMessageW(mic_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));if(inputs[i].key==selected_mic) mic_selection=i-loopbacks+1;}
        SendMessageW(mic_,CB_SETCURSEL,mic_selection,0);ListView_DeleteAllItems(list_);
        for(unsigned i=0;i<outputs.size();++i) {auto name=wide(outputs[i].name);LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=i;item.pszText=name.data();ListView_InsertItem(list_,&item);update_row(i);ListView_SetCheckState(list_,i,previous?std::find(checked.begin(),checked.end(),outputs[i].key)!=checked.end():matrix.output_connected(i));}
        refreshing_=false;EnableWindow(edit_,FALSE);
    }
    float test_gain() const {int choice=int(SendMessageW(level_,CB_GETCURSEL,0,0));return float(std::pow(10.,(-36+6*std::clamp(choice,0,5))/20.)/.015);}
    void save_selection(std::ostream& out) const {
        if(!mic_) return;auto mic=mic_key();if(!mic.empty()) out<<"calibration_mic "<<std::quoted(mic)<<"\n";
        for(unsigned i=0;i<outputs_.size();++i) if(ListView_GetCheckState(list_,i)) out<<"calibration_output "<<std::quoted(outputs_[i].key)<<"\n";
    }
    void load_selection(std::istream& in) {
        std::string line;std::vector<std::string> checked;bool found=false;
        while(std::getline(in,line)) {std::istringstream fields(line);std::string kind,key;fields>>kind>>std::quoted(key);
            if(kind=="calibration_mic") for(unsigned i=first_input_;i<inputs_.size();++i) if(inputs_[i].key==key) SendMessageW(mic_,CB_SETCURSEL,i-first_input_+1,0);
            if(kind=="calibration_output") {checked.push_back(key);found=true;}}
        if(found) for(unsigned i=0;i<outputs_.size();++i) ListView_SetCheckState(list_,i,std::find(checked.begin(),checked.end(),outputs_[i].key)!=checked.end());
    }
    std::string mic_key() const {int index=int(SendMessageW(mic_,CB_GETCURSEL,0,0))-1;return index>=0&&first_input_+index<inputs_.size()?inputs_[first_input_+index].key:std::string{};}
    unsigned microphone() const {int index=int(SendMessageW(mic_,CB_GETCURSEL,0,0))-1;if(index<0) throw std::runtime_error("計測マイクを選択してください");return unsigned(index);}
    std::vector<unsigned> selected_outputs() const {std::vector<unsigned> result;for(unsigned i=0;i<outputs_.size();++i) if(ListView_GetCheckState(list_,i)) result.push_back(i);if(result.size()<2) throw std::runtime_error("揃える出力を2チャンネル以上チェックしてください");return result;}
    PortDelays candidate() const {if(selected_>=0) valid_delay(utf8(text(edit_).c_str()));return values_;}
    void notify(NMHDR* header) {
        if(header->hwndFrom!=list_||header->code!=LVN_ITEMCHANGED||refreshing_) return;
        auto item=reinterpret_cast<NMLISTVIEW*>(header);
        if((item->uNewState&LVIS_SELECTED)&&!(item->uOldState&LVIS_SELECTED)) {if(selected_>=0) valid_delay(utf8(text(edit_).c_str()));selected_=item->iItem;refreshing_=true;SetWindowTextW(edit_,label(values_[outputs_[selected_].key]).c_str());EnableWindow(edit_,TRUE);refreshing_=false;}
    }
    void command(unsigned id,unsigned notification) {
        if(refreshing_||selected_<0) return;
        if(id==minus_id||id==plus_id) {auto value=std::clamp(valid_delay(utf8(text(edit_).c_str()))+(id==plus_id?.1:-.1),0.,2000.);SetWindowTextW(edit_,label(value).c_str());return;}
        if(id!=edit_id||notification!=EN_CHANGE) return;
        double value;try {value=valid_delay(utf8(text(edit_).c_str()));}catch(...) {SetWindowTextW(note_,L"追加遅延は0〜2000 msで入力してください");return;}
        values_[outputs_[selected_].key]=value;update_row(selected_);
        if(SendMessageW(link_,BM_GETCHECK,0,0)==BST_CHECKED) {auto key=outputs_[selected_].key;auto colon=key.rfind(':');unsigned channel=std::stoul(key.substr(colon+1));auto partner=key.substr(0,colon+1)+std::to_string(channel^1);
            for(unsigned i=0;i<outputs_.size();++i) if(outputs_[i].key==partner) {values_[partner]=value;update_row(i);}}
        SetWindowTextW(note_,L"編集中 / 手動値を適用すると保存 / 主ASIOのクロックは維持");
    }
    void progress(const std::string& message) {SetWindowTextW(note_,wide(message).c_str());}
    void results(const std::vector<unsigned>& selected,const std::vector<unsigned>& measured,const std::vector<unsigned>& corrections,unsigned rate) {
        for(unsigned i=0;i<selected.size();++i) {unsigned row=selected[i];values_[outputs_[row].key]=corrections[i]*1000./rate;update_row(row);auto result=label(measured[i]*1000./rate)+L" ms（入力を含む）";ListView_SetItemText(list_,row,2,result.data());}
        if(selected_>=0) {refreshing_=true;SetWindowTextW(edit_,label(values_[outputs_[selected_].key]).c_str());refreshing_=false;}
    }
    void self_test() {
        if(outputs_.size()<2) throw std::runtime_error("Delay UI test requires two outputs");
        auto previous_level=SendMessageW(level_,CB_GETCURSEL,0,0);SendMessageW(level_,CB_SETCURSEL,5,0);
        if(std::abs(test_gain()*.015f-std::pow(10.,-6./20.))>1e-6) throw std::runtime_error("Louder calibration level mapping failed");
        SendMessageW(level_,CB_SETCURSEL,previous_level,0);
        auto previous=values_;selected_=0;SetWindowTextW(edit_,L"12.5");
        if(values_[outputs_[0].key]!=12.5) throw std::runtime_error("Delay edit did not update named row");
        auto key=outputs_[0].key;auto colon=key.rfind(':');auto partner=key.substr(0,colon+1)+std::to_string(std::stoul(key.substr(colon+1))^1);
        for(auto& output:outputs_) if(output.key==partner&&values_[partner]!=12.5) throw std::runtime_error("Stereo delay link failed");
        SetWindowTextW(edit_,L"nan");bool rejected=false;try {candidate();}catch(...) {rejected=true;}
        if(!rejected) throw std::runtime_error("Invalid UI delay accepted");
        refreshing_=true;values_=previous;for(unsigned i=0;i<outputs_.size();++i) update_row(i);SetWindowTextW(edit_,std::to_wstring(values_[outputs_[0].key]).c_str());selected_=-1;refreshing_=false;
        auto original_outputs=outputs_;std::stringstream original;save_selection(original);
        SendMessageW(mic_,CB_SETCURSEL,1,0);auto microphone_key=mic_key();
        for(unsigned i=0;i<outputs_.size();++i) ListView_SetCheckState(list_,i,i==0||i+1==outputs_.size());
        std::stringstream saved_selection;save_selection(saved_selection);
        auto reversed=outputs_;std::reverse(reversed.begin(),reversed.end());RoutingMatrix dummy(unsigned(inputs_.size()),unsigned(outputs_.size()),first_input_);
        auto input_copy=inputs_;refresh(input_copy,first_input_,reversed,previous,dummy);
        if(mic_key()!=microphone_key||selected_outputs()!=std::vector<unsigned>({0,unsigned(outputs_.size()-1)})) throw std::runtime_error("Calibration selection lost after inventory reorder");
        for(unsigned i=0;i<outputs_.size();++i) ListView_SetCheckState(list_,i,FALSE);load_selection(saved_selection);
        if(mic_key()!=microphone_key||selected_outputs().size()!=2) throw std::runtime_error("Calibration selection persistence failed");
        refresh(input_copy,first_input_,original_outputs,previous,dummy);load_selection(original);
    }
};
}
