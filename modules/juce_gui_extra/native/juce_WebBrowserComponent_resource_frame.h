/* Internal parent/helper protocol. Both endpoints are from the same JUCE build. */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <optional>
namespace juce::WebViewResourceFrame
{
constexpr size_t headerSize = 24;
constexpr size_t maximumData = 64 * 1024 * 1024;
inline void put32(char* p, uint32_t v) { for (unsigned i=0;i<4;++i) p[i]=char((v>>(8*i))&255); }
inline uint32_t get32(const char* p) { uint32_t v=0; for(unsigned i=0;i<4;++i) v|=uint32_t(static_cast<unsigned char>(p[i]))<<(8*i); return v; }
struct View { int64_t id; const char* mime; uint32_t mimeSize; const char* data; uint32_t dataSize; };
inline std::optional<View> decode(const char* bytes, size_t size)
{
    if (size < headerSize || std::memcmp(bytes, "\0JBR", 4) != 0 || get32(bytes+4) != 1) return {};
    const auto mime=get32(bytes+16), data=get32(bytes+20);
    if (mime == 0 || mime > 256 || data > maximumData || uint64_t(headerSize)+mime+data != size) return {};
    for(uint32_t i=0;i<mime;++i) if(bytes[headerSize+i] < 32 || bytes[headerSize+i] > 126) return {};
    const uint64_t id=uint64_t(get32(bytes+8)) | (uint64_t(get32(bytes+12))<<32);
    if (id > INT64_MAX) return {};
    return View {static_cast<int64_t>(id),bytes+headerSize,mime,bytes+headerSize+mime,data};
}
inline std::vector<char> encode(int64_t id, const std::string& mime, const void* data, size_t size)
{
    if (id < 0 || mime.empty() || mime.size() > 256 || size > maximumData) return {};
    for(const auto ch : mime) if(ch < 32 || ch > 126) return {};
    std::vector<char> out(headerSize+mime.size()+size);
    std::memcpy(out.data(), "\0JBR", 4); put32(out.data()+4,1);
    put32(out.data()+8,uint32_t(id)); put32(out.data()+12,uint32_t(uint64_t(id)>>32));
    put32(out.data()+16,uint32_t(mime.size())); put32(out.data()+20,uint32_t(size));
    std::memcpy(out.data()+headerSize,mime.data(),mime.size());
    if(size) std::memcpy(out.data()+headerSize+mime.size(),data,size);
    return out;
}
}
