// SPDX-License-Identifier: MIT
// Minimal valid CNA avatar catalog files for tests, and handles to mutate them.
#pragma once
#include "CnaService/AvatarAssets.hpp"
#include "CnaService/Protocol.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace CnaService::Fixtures {
inline constexpr std::array<int,71> Parents{
    -1, 0, 0, 0, 0, 1, 2, 2, 3, 3, 1, 6, 5, 6, 5, 8, 5, 8, 5, 14, 12, 11, 16, 15, 14, 20, 20, 20, 22, 22, 22,
    25, 25, 25, 28, 28, 28, 33, 33, 33, 33, 33, 33, 33, 36, 36, 36, 36, 36, 36, 36, 37, 38, 39, 40, 43, 44,
    45, 46, 47, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60};
inline const std::array<const char*,71> Names{
    "Root", "BackLower", "HipLeft", "HipRight", "Slot04", "BackUpper", "KneeLeft", "Slot07", "KneeRight", "Slot09",
    "Slot10", "AnkleLeft", "CollarLeft", "Slot13", "Neck", "AnkleRight", "CollarRight", "Slot17", "Slot18", "Head",
    "ShoulderLeft", "ToeLeft", "ShoulderRight", "ToeRight", "Slot24", "ElbowLeft", "Slot26", "Slot27", "ElbowRight",
    "Slot29", "Slot30", "Slot31", "Slot32", "WristLeft", "Slot34", "Slot35", "WristRight", "FingerIndexLeft",
    "FingerMiddleLeft", "FingerRingLeft", "FingerSmallLeft", "PropLeft", "SpecialLeft", "FingerThumbLeft",
    "FingerIndexRight", "FingerMiddleRight", "FingerRingRight", "FingerSmallRight", "PropRight", "SpecialRight",
    "FingerThumbRight", "FingerIndex2Left", "FingerMiddle2Left", "FingerRing2Left", "FingerSmall2Left",
    "FingerThumb2Left", "FingerIndex2Right", "FingerMiddle2Right", "FingerRing2Right", "FingerSmall2Right",
    "FingerThumb2Right", "FingerIndex3Left", "FingerMiddle3Left", "FingerRing3Left", "FingerSmall3Left",
    "FingerThumb3Left", "FingerIndex3Right", "FingerMiddle3Right", "FingerRing3Right", "FingerSmall3Right",
    "FingerThumb3Right"};

/** A GLB under construction: glTF JSON plus its binary chunk. */
struct GlbParts {
    Json json;
    std::string bin;

    std::size_t view(const std::string& bytes)
    {
        while(bin.size()%4)bin.push_back('\0');
        json["bufferViews"].push_back(Json{{"buffer",0},{"byteOffset",bin.size()},{"byteLength",bytes.size()}});
        bin+=bytes;
        return json["bufferViews"].size()-1;
    }
    template<typename T>
    std::size_t accessor(const std::vector<T>& values,const char* type,int component,std::size_t count,bool normalized=false)
    {
        std::string bytes(values.size()*sizeof(T),'\0');
        std::memcpy(bytes.data(),values.data(),bytes.size());
        Json a{{"bufferView",view(bytes)},{"componentType",component},{"count",count},{"type",type}};
        if(normalized)a["normalized"]=true;
        json["accessors"].push_back(a);
        return json["accessors"].size()-1;
    }
    std::string build() const
    {
        auto text=json.dump();
        while(text.size()%4)text.push_back(' ');
        auto b=bin;
        while(b.size()%4)b.push_back('\0');
        std::string out("glTF",4);
        auto u32=[&](std::uint32_t v){for(int i=0;i<4;++i)out.push_back(static_cast<char>((v>>(8*i))&0xff));};
        u32(2);u32(static_cast<std::uint32_t>(12+8+text.size()+8+b.size()));
        u32(static_cast<std::uint32_t>(text.size()));u32(0x4E4F534A);out+=text;
        u32(static_cast<std::uint32_t>(b.size()));u32(0x004E4942);out+=b;
        return out;
    }
};

/** A valid avatar GLB: the rig (bind offsets scaled by `scale`), one skinned triangle named `tag`,
 * and `clips` animations. */
inline GlbParts avatarGlb(const std::string& tag,int clips=0,float scale=1.0f)
{
    GlbParts g;
    g.json=Json{{"asset",{{"version","2.0"}}},{"scene",0},{"scenes",Json::array({Json{{"nodes",Json::array({0})}}})},
                {"nodes",Json::array()},{"bufferViews",Json::array()},{"accessors",Json::array()}};
    for(int i=0;i<71;++i) {
        Json node{{"name",Names[static_cast<std::size_t>(i)]}};
        if(i)node["translation"]={0.0,0.01*scale*static_cast<double>(i%5+1),0.0};
        Json children=Json::array();
        for(int c=0;c<71;++c)if(Parents[static_cast<std::size_t>(c)]==i)children.push_back(c);
        if(!children.empty())node["children"]=children;
        g.json["nodes"].push_back(node);
    }
    Json joints=Json::array();
    for(int i=0;i<71;++i)joints.push_back(i);
    g.json["skins"]=Json::array({Json{{"joints",joints}}});
    const auto positions=g.accessor(std::vector<float>{0,0,0, 0.1f,0,0, 0,0.1f,0},"VEC3",5126,3);
    const auto normals=g.accessor(std::vector<float>{0,0,1, 0,0,1, 0,0,1},"VEC3",5126,3);
    const auto jointData=g.accessor(std::vector<std::uint8_t>{19,0,0,0, 19,0,0,0, 19,0,0,0},"VEC4",5121,3);
    const auto weights=g.accessor(std::vector<std::uint8_t>{255,0,0,0, 255,0,0,0, 255,0,0,0},"VEC4",5121,3,true);
    const auto indices=g.accessor(std::vector<std::uint16_t>{0,1,2},"SCALAR",5123,3);
    g.json["materials"]=Json::array({Json{{"name","m"},{"extras",{{"cnaTint","skin"}}},
                                           {"pbrMetallicRoughness",{{"baseColorFactor",{1,1,1,1}}}}}});
    g.json["meshes"]=Json::array({Json{{"name",tag},{"primitives",Json::array({Json{
        {"attributes",{{"POSITION",positions},{"NORMAL",normals},{"JOINTS_0",jointData},{"WEIGHTS_0",weights}}},
        {"indices",indices},{"material",0},{"mode",4}}})}}});
    g.json["nodes"].push_back(Json{{"name",tag},{"mesh",0},{"skin",0}});
    if(clips) {
        const auto times=g.accessor(std::vector<float>{0.0f,1.0f},"SCALAR",5126,2);
        const auto values=g.accessor(std::vector<float>{0,0,0,0, 0,0,0,1, 0,0,0,0, 0,0,0,0, 0,0,0,1, 0,0,0,0},"VEC4",5126,6);
        g.json["animations"]=Json::array();
        for(int c=0;c<clips;++c)
            g.json["animations"].push_back(Json{{"name","clip"+std::to_string(c)},
                {"samplers",Json::array({Json{{"input",times},{"output",values},{"interpolation","CUBICSPLINE"}}})},
                {"channels",Json::array({Json{{"sampler",0},{"target",{{"node",1},{"path","rotation"}}}}})},
                {"extras",{{"cnaExpressions",Json::array({Json::array({0,0,0,0,0,0})})}}}});
    }
    g.json["buffers"]=Json::array({Json{{"byteLength",g.bin.size()}}});
    return g;
}

inline void finish(GlbParts& g)
{
    while(g.bin.size()%4)g.bin.push_back('\0');
    g.json["buffers"]=Json::array({Json{{"byteLength",g.bin.size()}}});
}

/** A face atlas PNG header (1024 x 320) and a layout naming every expression state. */
inline std::string atlasPng() {
    // Signature and a whole IHDR chunk (1024 x 320, RGBA), its CRC included.
    return std::string("\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR\x00\x00\x04\x00\x00\x00\x01\x40\x08\x06\x00\x00\x00\x22\x7d\x0e\x48",33);
}
inline Json atlasLayout() {
    Json eyes=Json::object(), brows=Json::object(), mouths=Json::object();
    for(const char* s:{"Neutral","Sad","Angry","Confused","Laughing","Shocked","Happy","Yawning","Sleeping","LookUp","LookDown",
                       "LookLeft","LookRight","Blink"})eyes[s]={{"left",{0,1}},{"right",{2,3}}};
    for(const char* s:{"Neutral","Sad","Angry","Confused","Raised"})brows[s]={{"left",4},{"right",5}};
    for(const char* s:{"Neutral","Sad","Angry","Confused","Laughing","Shocked","Happy","PhoneticO","PhoneticAi","PhoneticEe",
                       "PhoneticFv","PhoneticW","PhoneticL","PhoneticDth"})mouths[s]=6;
    return Json{{"tileSize",64},{"columns",16},{"width",1024},{"height",320},{"eyes",eyes},{"eyebrows",brows},{"mouths",mouths}};
}
}
