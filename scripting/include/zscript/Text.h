#pragma once
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace zengine::script::text {
inline bool Scalar(char32_t c) { return c<=0x10ffff && !(c>=0xd800 && c<=0xdfff); }
inline std::string Encode(char32_t c) {
    if(!Scalar(c))throw std::invalid_argument("Invalid Unicode character");
    std::string s;
    if(c<0x80)s+=static_cast<char>(c);
    else if(c<0x800){s+=static_cast<char>(0xc0|(c>>6));s+=static_cast<char>(0x80|(c&63));}
    else if(c<0x10000){s+=static_cast<char>(0xe0|(c>>12));s+=static_cast<char>(0x80|((c>>6)&63));s+=static_cast<char>(0x80|(c&63));}
    else {s+=static_cast<char>(0xf0|(c>>18));s+=static_cast<char>(0x80|((c>>12)&63));s+=static_cast<char>(0x80|((c>>6)&63));s+=static_cast<char>(0x80|(c&63));}
    return s;
}
inline char32_t Next(std::string_view s,std::size_t& p) {
    if(p>=s.size())throw std::invalid_argument("Missing Unicode character");
    const auto first=static_cast<unsigned char>(s[p++]);char32_t c=first;unsigned n=0;
    if(first<0x80)return c;
    if(first>=0xc2 && first<=0xdf){n=1;c=first&31;}
    else if(first>=0xe0 && first<=0xef){n=2;c=first&15;}
    else if(first>=0xf0 && first<=0xf4){n=3;c=first&7;}
    else throw std::invalid_argument("Invalid UTF-8 text");
    for(unsigned i=0;i<n;++i){if(p>=s.size() || (static_cast<unsigned char>(s[p])&0xc0)!=0x80)throw std::invalid_argument("Invalid UTF-8 text");c=(c<<6)|(static_cast<unsigned char>(s[p++])&63);}
    if(!Scalar(c) || (n==1 && c<0x80) || (n==2 && c<0x800) || (n==3 && c<0x10000))throw std::invalid_argument("Invalid UTF-8 text");return c;
}
inline std::vector<std::size_t> Offsets(std::string_view s) {
    std::vector<std::size_t> offsets;std::size_t p=0;
    while(p<s.size()){offsets.push_back(p);Next(s,p);}offsets.push_back(p);return offsets;
}
inline char32_t Character(std::string_view s) {
    std::size_t p=0;const auto c=Next(s,p);if(p!=s.size())throw std::invalid_argument("Expected exactly one Unicode character");return c;
}
}
