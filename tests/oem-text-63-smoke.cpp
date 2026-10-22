// Host-only regression: g++ -std=c++17 -Wall -Wextra -Werror -Isrc ...
#include <cassert>
#include <cstdio>
#include <cstring>
#include "Helpers/OemText63.hpp"
int main() {
    OemText63::Payload p{};
    assert(OemText63::Encode("Michael Jackson","Can't Get Outta The Rain",p));
    assert(p.data[0]==0x63);
    const auto first=OemText63::FirstFrame(p);
    assert(first.length==8 && first.data[0]==0x10 && first.data[1]==p.size);
    assert(first.data[2]==0x63 && first.data[3]=='M');
    size_t pos=6; uint8_t seq=1;
    while(pos<p.size) {
        const auto f=OemText63::ConsecutiveFrame(p,pos,seq++);
        assert(f.length>=2 && f.length<=8);
        assert(f.data[0]==uint8_t(0x20|((seq-1)&0x0F)));
        for(uint8_t j=1;j<f.length;j++) assert(f.data[j]==p.data[pos+j-1]);
        pos+=f.length-1;
    }
    assert(pos==p.size);
    assert(!OemText63::Encode("","title",p));
    char longname[150]; memset(longname,'A',149);longname[149]=0;
    assert(OemText63::Encode(longname,longname,p) && p.size<=OemText63::Capacity);
    assert(OemText63::Encode("Priklad","Dvorak",p) && p.data[0]==0x63);
    assert(OemText63::Encode("Příliš","Název",p) && p.size<=OemText63::Capacity);
    std::puts("PASS: OEM type 0x63 payload bounds, first/consecutive frames and ASCII handling");
}
