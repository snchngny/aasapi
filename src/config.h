#pragma once
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <stdexcept>

namespace aggregate {
struct EndpointSpec { std::string id; unsigned channels=0; bool enabled=true; };
struct Config {
    unsigned sample_rate=48000,block_frames=256,queue_blocks=2;
    bool exclusive=false,simulation=false;
    std::string native_asio_clsid;
    unsigned pc_channels=2;
    std::string native_wasapi_endpoint;
    std::vector<unsigned> output_delay_ms;
    // Resolved routing-port delays, in samples; runtime only, saved by stable port ID.
    std::vector<unsigned> output_delay_frames;
    std::vector<EndpointSpec> capture,playback;
    bool native_endpoint_matches(const std::string& id) const {std::istringstream values(native_wasapi_endpoint);std::string item;while(std::getline(values,item,',')) if(item==id) return true;return false;}

};
inline std::string trim(std::string value) {
    auto a=value.find_first_not_of(" \t\r\n");
    if(a==std::string::npos) return {};
    return value.substr(a,value.find_last_not_of(" \t\r\n")-a+1);
}
inline Config load_config(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if(!stream) throw std::runtime_error("Cannot read configuration: "+path.string());
    std::map<std::string,std::map<std::string,std::string>> sections;
    std::string section,line;
    while(std::getline(stream,line)) {
        line=trim(line);
        if(line.empty() || line[0]=='#' || line[0]==';') continue;
        if(line[0]=='[' && line.back()==']') { section=line.substr(1,line.size()-2); continue; }
        auto equals=line.find('=');
        if(equals==std::string::npos) throw std::runtime_error("Invalid configuration line");
        auto key=trim(line.substr(0,equals));
        if(!sections[section].emplace(key,trim(line.substr(equals+1))).second)
            throw std::runtime_error("Duplicate configuration key");
    }
    Config config;
    auto integer=[](const std::string& text) {
        size_t n=0; auto value=std::stoul(text,&n);
        if(n!=text.size() || value>1000000) throw std::runtime_error("Invalid integer");
        return unsigned(value);
    };
    auto boolean=[](const std::string& text) {
        if(text=="true") return true;
        if(text=="false") return false;
        throw std::runtime_error("Expected true or false");
    };
    for(auto& [name,items]:sections) {
        if(name=="engine") {
            for(auto& [key,value]:items) {
                if(key=="sample_rate") config.sample_rate=integer(value);
                else if(key=="block_frames") config.block_frames=integer(value);
                else if(key=="queue_blocks") config.queue_blocks=integer(value);
                else if(key=="exclusive") config.exclusive=boolean(value);
                else if(key=="simulation") config.simulation=boolean(value);
                else if(key=="pc_channels") config.pc_channels=integer(value);
                else if(key=="native_wasapi_endpoint") config.native_wasapi_endpoint=value;
                else if(key=="output_delay_ms") { std::istringstream values(value); std::string item; while(std::getline(values,item,',')) config.output_delay_ms.push_back(integer(trim(item))); }
                else if(key=="native_asio_clsid") config.native_asio_clsid=value;
                else throw std::runtime_error("Unknown engine option: "+key);
            }
        } else if(name.rfind("capture.",0)==0 || name.rfind("playback.",0)==0) {
            bool input=name.rfind("capture.",0)==0;
            unsigned index=integer(name.substr(input?8:9));
            auto& endpoints=input?config.capture:config.playback;
            if(index>=8) throw std::runtime_error("At most 8 endpoints per direction");
            endpoints.resize(std::max(endpoints.size(),size_t(index+1)));
            for(auto& [key,value]:items) {
                if(key=="id") endpoints[index].id=value;
                else if(key=="channels") endpoints[index].channels=integer(value);
                else throw std::runtime_error("Unknown endpoint option: "+key);
            }
        } else throw std::runtime_error("Unknown configuration section: "+name);
    }
    if(config.sample_rate<8000 || config.sample_rate>192000) throw std::runtime_error("sample_rate must be 8000..192000");
    if(config.pc_channels<1 || config.pc_channels>64) throw std::runtime_error("pc_channels must be 1..64");
    if(config.output_delay_ms.size()>128 || std::any_of(config.output_delay_ms.begin(),config.output_delay_ms.end(),[](unsigned n){return n>2000;})) throw std::runtime_error("output_delay_ms must contain at most 128 values in 0..2000");
    if(config.block_frames<32 || config.block_frames>2048)
        throw std::runtime_error("block_frames must be from 32 to 2048");
    if(config.queue_blocks<2 || config.queue_blocks>8) throw std::runtime_error("queue_blocks must be 2..8");
    if(config.simulation && !config.native_asio_clsid.empty()) throw std::runtime_error("Simulation cannot open native ASIO");
    if(config.playback.empty() && config.native_asio_clsid.empty()) throw std::runtime_error("playback.0 or native_asio_clsid is required as clock master");
    for(auto* endpoints:{&config.capture,&config.playback}) {
        unsigned total=0;
        std::vector<std::string> used;
        for(auto& endpoint:*endpoints) {
            if(endpoint.id.empty() || endpoint.channels<1 || endpoint.channels>64)
                throw std::runtime_error("Endpoint id and channels (1..64) required");
            if(std::find(used.begin(),used.end(),endpoint.id)!=used.end())
                throw std::runtime_error("Duplicate endpoint in same direction");
            used.push_back(endpoint.id); total+=endpoint.channels;
        }
        if(total>128) throw std::runtime_error("At most 128 channels per direction");
    }
    return config;
}
}
