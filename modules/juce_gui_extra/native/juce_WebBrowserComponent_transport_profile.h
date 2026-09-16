#pragma once
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace juce::WebViewTransportProfile
{
inline bool enabled() { static const bool e=[] { const auto* p=std::getenv("SOLSTICE_TRANSPORT_PROFILE"); return p && std::strcmp(p,"1")==0; }(); return e; }
inline double now() { return enabled() ? std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count() : 0; }
struct Summary {
    std::array<double,512> samples{};
    size_t count=0, bytes=0;
    double start=now();
    void add(const char* label,double elapsed,size_t size=0) {
        if(!enabled()) return;
        samples[count++ % samples.size()]=elapsed; bytes+=size;
        const auto end=now(); if(end-start<10000) return;
        auto sorted=samples; const auto n=std::min(count,samples.size());
        std::sort(sorted.begin(),sorted.begin()+n);
        const auto p=[&](double x){return sorted[std::min(n-1,size_t(x*n))];};
        std::fprintf(stderr,"[transport-helper] %s count=%zu bytes=%zu interval_ms=%.0f recent_n=%zu p50_ms=%.3f p95_ms=%.3f p99_ms=%.3f\n",label,count,bytes,end-start,n,p(.5),p(.95),p(.99));
        count=bytes=0;start=end;
    }
};
}
