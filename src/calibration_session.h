#pragma once
#include "engine.h"
#include "endpoint_status.h"
#include "latency_measurement.h"
#include "output_delay.h"
#include "delay_settings.h"
#include <functional>
#include <deque>
namespace aggregate {
inline bool calibration_queues_stable(const std::deque<std::vector<double>>& history,unsigned rate) {
    if(history.size()<20) return false;
    double center=(history.size()-1)/2.,denominator=0;
    for(unsigned i=0;i<history.size();++i) denominator+=(i-center)*(i-center);
    for(unsigned c=0;c<history.front().size();++c) {double slope=0;
        for(unsigned i=0;i<history.size();++i) slope+=(i-center)*history[i][c];
        // Twenty seconds of one-second samples; each path may move by at most
        // 0.02 ms per second before sequential output measurements begin.
        if(std::abs(slope/denominator)>rate*.00002) return false;
    }return true;
}
struct CalibrationResult {std::vector<unsigned> measured,corrections,residual;};
// Verify the actual acoustic return after each adjustment, never an estimated
// residual. Bounded retries prevent noisy measurements from running forever.
template<class Measure> CalibrationResult refine_calibration(const std::vector<unsigned>& measured,unsigned rate,Measure measure,const std::function<void(const std::string&)>& report) {
    CalibrationResult result;result.measured=measured;result.corrections=align_delays(measured,rate);
    const unsigned tolerance=rate/10000; // At most 0.1 ms, rounded down to samples.
    for(unsigned attempt=0;attempt<=5;++attempt) {
        result.residual=measure(result.corrections,attempt);
        if(result.residual.size()!=measured.size()) throw std::runtime_error("再測定の出力数が一致しません。補正値は変更していません");
        auto [low,high]=std::minmax_element(result.residual.begin(),result.residual.end());
        unsigned spread=*high-*low;
        report("補正確認 "+std::to_string(attempt+1)+"回目 / 出力差 "+std::to_string(spread*1000./rate)+" ms / 目標0.1 ms以内");
        if(spread<=tolerance) return result;
        if(attempt==5) throw std::runtime_error("追加補正5回後も出力差 "+std::to_string(spread*1000./rate)+" msで目標0.1 ms以内に収まりません。測定の揺れ・機器の同期を確認してください。補正値は変更していません");
        // Add the observed residual, then remove common delay. This also allows
        // a previously delayed output to become the new reference after drift.
        auto adjustment=align_delays(result.residual,rate);
        std::vector<uint64_t> next(measured.size());
        for(unsigned i=0;i<next.size();++i) next[i]=uint64_t(result.corrections[i])+adjustment[i];
        uint64_t minimum=*std::min_element(next.begin(),next.end());
        for(unsigned i=0;i<next.size();++i) {
            if(next[i]-minimum>uint64_t(rate)*2) throw std::runtime_error("追加補正が2000 msを超えています。補正値は変更していません");
            result.corrections[i]=unsigned(next[i]-minimum);
        }
    }
    throw std::runtime_error("補正確認が完了しません");
}
struct CalibrationCapture {
    unsigned inputs,outputs,input,rate,position=0;
    std::vector<unsigned> selected;
    std::vector<float> reference,captured;
    OutputDelay delay;
    std::atomic<float> peak{0},maximum_peak{0};
    std::atomic<unsigned> progress{0};
    std::atomic<bool> recording{true},idle{false};
    std::atomic<uint64_t> idle_frames{0};
    CalibrationCapture(unsigned in,unsigned out,unsigned mic,unsigned hz,const std::vector<unsigned>& channels,const std::vector<unsigned>& corrections,float gain=1)
        :inputs(in),outputs(out),input(mic),rate(hz),selected(channels),reference(calibration_signal(hz)),captured(size_t(hz)*(2+channels.size()*12)),delay(out,hz,{},corrections) {for(auto& sample:reference) sample*=gain;}
    static void process(void* user,const float* input,float* output,unsigned frames) {
        auto& self=*static_cast<CalibrationCapture*>(user);std::fill_n(output,size_t(frames)*self.outputs,0.f);
        if(!self.recording.load(std::memory_order_acquire)) {self.idle_frames.fetch_add(frames);self.idle.store(true,std::memory_order_release);return;}
        self.idle=false;float peak=0;
        for(unsigned f=0;f<frames;++f) {
            unsigned at=self.position++;float value=input[size_t(f)*self.inputs+self.input];peak=std::max(peak,std::abs(value));
            if(at<self.captured.size()) self.captured[at]=value;
            if(at>=2*self.rate) {unsigned slot=(at-2*self.rate)/(3*self.rate),offset=(at-2*self.rate)%(3*self.rate);
                if(slot<self.selected.size()*4&&offset<self.reference.size()) output[size_t(f)*self.outputs+self.selected[slot/4]]=self.reference[offset];}
        }
        self.delay.process(output,frames,false);self.peak=std::max(self.peak.load(),peak);self.maximum_peak=std::max(self.maximum_peak.load(),peak);self.progress=self.position;
    }
    std::vector<unsigned> analyze(HANDLE cancel=nullptr,const std::function<void(const std::string&)>& report={}) const {
        std::vector<unsigned> result;
        for(unsigned c=0;c<selected.size();++c) {std::vector<unsigned> trials;
            for(unsigned trial=0;trial<3;++trial) {if(cancel&&WaitForSingleObject(cancel,0)!=WAIT_TIMEOUT) throw std::runtime_error("計測を中止しました。補正値は変更していません");try {auto measurement=measure_delay(reference,captured,rate*(2+(c*4+trial+1)*3),rate*2);trials.push_back(measurement.frames);if(report) report("出力ch "+std::to_string(selected[c]+1)+" / "+std::to_string(trial+1)+"回目 / 到達 "+std::to_string(measurement.frames*1000./rate)+" ms / 相関 "+std::to_string(measurement.correlation));}
                catch(const std::exception& e) {float trial_peak=0;unsigned first=rate*(2+(c*4+trial+1)*3);for(unsigned j=first;j<first+rate*2+reference.size();++j) trial_peak=std::max(trial_peak,std::abs(captured[j]));throw std::runtime_error("出力ch "+std::to_string(selected[c]+1)+" / "+std::to_string(trial+1)+"回目: "+e.what()+" / マイク最大 "+std::to_string(trial_peak>0?20*std::log10(trial_peak):-120)+" dBFS");}}
            std::sort(trials.begin(),trials.end());
            if(trials.back()-trials.front()>std::max(4u,rate/500)) throw std::runtime_error("出力ch "+std::to_string(selected[c]+1)+" / 3回の測定が不安定: "+std::to_string(trials[0]*1000./rate)+", "+std::to_string(trials[1]*1000./rate)+", "+std::to_string(trials[2]*1000./rate)+" ms（許容差2 ms）。機器の同期・入力の音声処理・マイク位置による変動を確認する必要があります。補正値は変更していません");
            result.push_back(trials[1]);
        }return result;
    }
};
inline CalibrationResult calibrate_outputs(const Config& config,unsigned input,const std::vector<unsigned>& selected,HANDLE cancel,const std::function<void(const std::string&)>& report,float gain=1,unsigned warmup_seconds=60) {
    Engine engine(config);
    if(input>=engine.inputs()||selected.size()<2) throw std::runtime_error("マイクと2つ以上の出力チャンネルを選択してください");
    for(auto c:selected) if(c>=engine.outputs()) throw std::runtime_error("出力一覧が変わりました。機器一覧を更新してください");
    CalibrationCapture capture(engine.inputs(),engine.outputs(),input,config.sample_rate,selected,{},gain);
    struct StopEngine {Engine& engine;~StopEngine(){engine.stop();}} stop_engine{engine};
    report("マイク・対象出力を初期化中 / 入力ch "+std::to_string(input+1));
    engine.prepare(CalibrationCapture::process,&capture);engine.observe_outputs(true);
    for(auto* endpoints:{&config.capture,&config.playback}) for(auto& endpoint:*endpoints) if(endpoint.enabled&&!config.simulation) {
        auto state=endpoint_status(endpoint.id);
        if(state.available&&(state.muted||state.volume==0)) throw std::runtime_error("選択端子がWindowsでミュートまたは音量0です: "+endpoint.id);
    }
    if(engine.health_flags()&2) throw std::runtime_error("選択したマイクまたは出力を開けません。抜き差し後の機器一覧とWindowsのマイク権限を確認してください");
    capture.recording=false;engine.start();
    // Keep clocks/resamplers running through analysis and verification. Restarting
    // here changes queue latency and invalidates the measured correction.
    std::deque<std::vector<double>> queue_history;unsigned stable_seconds=0;bool settled=false;
    for(unsigned tick=0;tick<(warmup_seconds+120)*5;++tick) {
        if(WaitForSingleObject(cancel,200)!=WAIT_TIMEOUT) throw std::runtime_error("計測を中止しました。補正値は変更していません");
        if(tick%5==0) {queue_history.push_back(engine.clock_queue_levels());if(queue_history.size()>20) queue_history.pop_front();
            stable_seconds=tick/5>=warmup_seconds&&calibration_queues_stable(queue_history,config.sample_rate)?stable_seconds+1:0;
            if(stable_seconds>=5) {report("同期安定を確認 / "+std::to_string(tick/5)+"秒");settled=true;break;}}
        if(tick%25==0) {report("同期準備の診断: "+engine.diagnostics());report("同期を安定化中 / "+std::to_string(tick/5)+"秒 / 最低 "+std::to_string(warmup_seconds)+"秒、変動が落ち着くまで待機");}
    }
    if(!settled) throw std::runtime_error("同期バッファが安定しないため計測を開始できません。補正値は変更していません");
    if(!capture.idle_frames) throw std::runtime_error("音声クロックが進みません");
    auto idle=[&] {
        capture.recording.store(false,std::memory_order_release);auto until=GetTickCount64()+3000;
        while(!capture.idle.load(std::memory_order_acquire)) {if(GetTickCount64()>until) throw std::runtime_error("音声クロックの応答がありません");Sleep(1);}
    };
    auto run=[&](const std::vector<unsigned>& corrections,const char* stage) {
        idle();
        capture.position=0;capture.progress=0;capture.peak=0;capture.maximum_peak=0;std::fill(capture.captured.begin(),capture.captured.end(),0.f);
        capture.delay=OutputDelay(engine.outputs(),config.sample_rate,{},corrections);
        capture.idle=false;capture.recording.store(true,std::memory_order_release);auto last_progress=GetTickCount64();unsigned previous_progress=0;
        try {while(capture.progress.load()<capture.captured.size()) {
            if(WaitForSingleObject(cancel,200)!=WAIT_TIMEOUT) throw std::runtime_error("計測を中止しました。補正値は変更していません");
            auto position=capture.progress.load();auto now=GetTickCount64();
            if(position!=previous_progress) {last_progress=now;previous_progress=position;}
            if(now-last_progress>3000) throw std::runtime_error("音声クロックが3秒以上進みません。主ASIOと機器の接続を確認してください");
            unsigned slot=position>=2*capture.rate?(position-2*capture.rate)/(3*capture.rate):0;
            std::string current=position>=capture.captured.size()?"収録完了":position<2*capture.rate?"準備":("出力ch "+std::to_string(selected[std::min(unsigned(selected.size()-1),slot/4)]+1)+" / "+(slot%4==0?"予備音":std::to_string(slot%4)+"回目"));
            report(std::string(stage)+" / "+current+" / "+std::to_string(std::min(100u,unsigned(capture.progress.load()*100ull/capture.captured.size())))+"% / マイク "+std::to_string(capture.peak.load()>0?20*std::log10(std::max(1e-6f,capture.peak.exchange(0))):-120)+" dBFS");
        }} catch(...) {engine.stop();throw;}
        idle();report("Windows端子への送出ピーク: "+engine.output_levels());report("計測時の同期診断: "+engine.diagnostics());if(engine.health_flags()) throw std::runtime_error("機器・バッファの異常を検出したため計測結果を適用しません");
        report(std::string(stage)+" / 信号を解析中");return capture.analyze(cancel,report);
    };
    auto measured=run({},"試験音で計測中");
    auto result=refine_calibration(measured,config.sample_rate,[&](const std::vector<unsigned>& values,unsigned attempt) {
        std::vector<unsigned> corrections(engine.outputs());for(unsigned i=0;i<selected.size();++i) corrections[selected[i]]=values[i];
        std::string stage="補正後を再計測中 / 確認"+std::to_string(attempt+1)+"回目";
        return run(corrections,stage.c_str());
    },report);
    if(WaitForSingleObject(cancel,0)!=WAIT_TIMEOUT) throw std::runtime_error("計測を中止しました。補正値は変更していません");
    engine.stop();
    return result;
}
}
