// Native self-extracting launcher. Payload and child are native C++; no Python.
#include "picosha2.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif
namespace fs = std::filesystem;
static std::vector<unsigned char> read_bytes(const fs::path& p) {
    std::ifstream f(p,std::ios::binary);
    if(!f)throw std::runtime_error("Cannot read bundle/cache file");
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)),{});
}
static uint64_t number(const std::vector<unsigned char>& data,size_t& pos,size_t width) {
    if(pos>data.size()||width>data.size()-pos)throw std::runtime_error("Truncated bundle");
    uint64_t value=0;
    for(size_t j=0;j<width;++j)value|=uint64_t(data[pos++])<<(8*j);
    return value;
}
static fs::path executable() {
#ifdef _WIN32
    std::vector<wchar_t> path(32768);
    auto n=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
    if(!n||n>=path.size())throw std::runtime_error("Cannot locate executable");
    return fs::path(std::wstring(path.data(),n));
#elif defined(__APPLE__)
    uint32_t n=0;_NSGetExecutablePath(nullptr,&n);std::vector<char> path(n);
    if(_NSGetExecutablePath(path.data(),&n))throw std::runtime_error("Cannot locate executable");
    return fs::weakly_canonical(path.data());
#else
    return fs::canonical("/proc/self/exe");
#endif
}
static std::string hex(const std::vector<unsigned char>& bytes) {
    return picosha2::bytes_to_hex_string(bytes);
}
struct Entry { fs::path path; uint64_t size; uint32_t mode; std::string hash; size_t offset; };
static fs::path unpack(const fs::path& file) {
    auto data=read_bytes(file);
    const std::string magic="LABEND01";
    size_t footer=std::string::npos;
    // Last footer normally ends at EOF. Bounded backward search also permits signatures.
    size_t begin=data.size()>1048576?data.size()-1048576:0;
    if(data.size()>=56)for(size_t i=data.size()-56;;--i) {
        if(std::equal(magic.begin(),magic.end(),data.begin()+i)){footer=i;break;}
        if(i==begin)break;
    }
    if(footer==std::string::npos)throw std::runtime_error("Embedded payload footer is missing");
    size_t pos=footer+8;
    uint64_t offset=number(data,pos,8),size=number(data,pos,8);
    if(offset>footer||size<12||size>footer-offset||offset+size!=footer)throw std::runtime_error("Invalid payload range");
    std::vector<unsigned char> expected(data.begin()+pos,data.begin()+pos+32);
    std::string id=hex(expected);
    std::string actual=picosha2::hash256_hex_string(data.begin()+offset,data.begin()+offset+size);
    if(actual!=id)throw std::runtime_error("Embedded payload SHA256 mismatch");
    pos=size_t(offset);
    if(std::string(data.begin()+pos,data.begin()+pos+8)!="LABSFX01")throw std::runtime_error("Invalid payload");
    pos+=8;auto count=number(data,pos,4);
    if(count>512)throw std::runtime_error("Too many embedded files");
    std::vector<Entry> entries;
    for(uint64_t i=0;i<count;++i) {
        auto length=number(data,pos,4),file_size=number(data,pos,8),mode=number(data,pos,4);
        if(length>4096||pos+32+length>footer)throw std::runtime_error("Invalid entry header");
        std::vector<unsigned char> h(data.begin()+pos,data.begin()+pos+32);pos+=32;
        std::string name(data.begin()+pos,data.begin()+pos+length);pos+=size_t(length);
        auto path=fs::u8path(name);
        if(name.empty()||path.is_absolute()||path.has_root_name()||name.find(':')!=std::string::npos||
           name.find('\\')!=std::string::npos||name.find('\0')!=std::string::npos)
            throw std::runtime_error("Unsafe embedded filename");
        for(const auto& part:path)if(part==".."||part==".")throw std::runtime_error("Unsafe embedded path");
        if(pos>footer||file_size>footer-pos)throw std::runtime_error("Truncated embedded file");
        entries.push_back({path,file_size,uint32_t(mode),hex(h),pos});pos+=size_t(file_size);
    }
    if(pos!=footer)throw std::runtime_error("Trailing payload bytes");
    fs::path root;
#ifdef _WIN32
    wchar_t location[32768];
    auto n=GetEnvironmentVariableW(L"LABYRINTH_CACHE_DIR",location,32768);
    if(n&&n<32768)root=location;
    else {
        n=GetEnvironmentVariableW(L"LOCALAPPDATA",location,32768);
        root=(n&&n<32768)?fs::path(location):fs::temp_directory_path();
        root/="LabyrinthSingle";
    }
    auto pid=GetCurrentProcessId();
#else
    const char* custom=std::getenv("LABYRINTH_CACHE_DIR");
    root=custom?fs::path(custom):fs::temp_directory_path()/"LabyrinthSingle";
    auto pid=getpid();
#endif
    fs::create_directories(root);
    fs::path cache=root/id.substr(0,24);
    auto valid=[&](const fs::path& folder) {
        try {
            for(const auto& e:entries) {
                auto path=folder/e.path;
                if(!fs::is_regular_file(path)||fs::file_size(path)!=e.size)return false;
                if(picosha2::hash256_hex_string(read_bytes(path))!=e.hash)return false;
            }
            return true;
        }catch(...){return false;}
    };
    if(valid(cache))return cache;
    fs::path staging=root/(id.substr(0,24)+".tmp."+std::to_string(pid));
    if(fs::exists(staging))staging+=std::to_string(std::rand());
    fs::create_directories(staging);
    try {
        for(const auto& e:entries) {
            auto path=staging/e.path;fs::create_directories(path.parent_path());
            std::ofstream out(path,std::ios::binary);
            out.write(reinterpret_cast<const char*>(data.data()+e.offset),std::streamsize(e.size));
            out.close();if(!out)throw std::runtime_error("Cannot write extracted file");
#ifndef _WIN32
            fs::permissions(path,fs::perms(e.mode&0777),fs::perm_options::replace);
#endif
        }
        if(!valid(staging))throw std::runtime_error("Extracted file verification failed");
        std::error_code error;
        fs::rename(staging,cache,error);
        if(!error)return cache;
        if(valid(cache)){fs::remove_all(staging);return cache;}
        // Never delete a pre-existing cache, which another running process may use.
        return staging;
    }catch(...){fs::remove_all(staging);throw;}
}
#ifdef _WIN32
static std::wstring quote(const std::wstring& arg) {
    std::wstring out=L"\"";size_t slashes=0;
    for(wchar_t ch:arg) {
        if(ch==L'\\'){++slashes;continue;}
        if(ch==L'"'){out.append(slashes*2+1,L'\\');out+=ch;}
        else {out.append(slashes,L'\\');out+=ch;}
        slashes=0;
    }
    out.append(slashes*2,L'\\');out+=L'"';return out;
}
static fs::path sidecar_config(const fs::path& launcher, const fs::path& cache) {
    auto path = launcher.parent_path() / L"config" / L"labyrinth.json";
    if (!fs::is_regular_file(path)) {
        fs::create_directories(path.parent_path());
        std::error_code error;
        fs::copy_file(cache / L"config" / L"default.json", path, fs::copy_options::skip_existing, error);
        if (error || !fs::is_regular_file(path))
            throw std::runtime_error("Cannot initialize config/labyrinth.json beside the launcher; use --config PATH");
    }
    return path;
}
int wmain(int argc,wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);SetConsoleCP(CP_UTF8);
    try {
        auto launcher=executable();auto cache=unpack(launcher);auto child=cache/L"labyrinth_server.exe";
        std::wstring command=quote(child.wstring());
        bool config=false, help=false;
        for(int i=1;i<argc;++i) {
            std::wstring arg=argv[i];
            if(arg==L"--config") config=true;
            if(arg==L"--help"||arg==L"-h") help=true;
        }
        if(!config&&!help) command+=L" --config "+quote(sidecar_config(launcher,cache).wstring());
        for(int i=1;i<argc;++i){command+=L" ";command+=quote(argv[i]);}
        std::vector<wchar_t> buffer(command.begin(),command.end());buffer.push_back(0);
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
        if(!CreateProcessW(child.c_str(),buffer.data(),nullptr,nullptr,TRUE,0,nullptr,nullptr,&startup,&process))
            throw std::runtime_error("Cannot start native service; Windows error "+std::to_string(GetLastError()));
        CloseHandle(process.hThread);WaitForSingleObject(process.hProcess,INFINITE);
        DWORD code=1;GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hProcess);
        return int(code);
    }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
}
#else
// Host test driver exercises the same parser, integrity checks, extraction and cache.
int main(int argc,char** argv) {
    try {
        if(argc<2)throw std::runtime_error("Pass the Windows bundle path for extraction verification");
        auto cache=unpack(fs::u8path(argv[1]));
        std::cout<<cache.string()<<std::endl;return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
}
#endif
