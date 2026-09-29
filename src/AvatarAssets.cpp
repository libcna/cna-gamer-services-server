// SPDX-License-Identifier: MIT
// Import-time validation of CNA avatar catalog files. A service must not publish what a compliant
// client will refuse, so this checks the same contract the clients enforce, independently.
#include "CnaService/AvatarAssets.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace CnaService {
namespace {
// XNA's avatar skeleton: the parent of every slot, and the joint name a CNA asset uses for it.
constexpr std::array<int,AvatarJointCount> Parents{
    -1, 0, 0, 0, 0, 1, 2, 2, 3, 3, 1, 6, 5, 6, 5, 8, 5, 8, 5, 14, 12, 11, 16, 15, 14, 20, 20, 20, 22, 22, 22,
    25, 25, 25, 28, 28, 28, 33, 33, 33, 33, 33, 33, 33, 36, 36, 36, 36, 36, 36, 36, 37, 38, 39, 40, 43, 44,
    45, 46, 47, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60};
constexpr std::array<std::string_view,AvatarJointCount> Names{
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
constexpr std::array<std::string_view,8> Tints{"none","skin","hair","eyes","top","bottom","shoes","accessory"};
constexpr std::array<std::string_view,5> Features{"eyeLeft","eyeRight","eyebrowLeft","eyebrowRight","mouth"};
constexpr std::array<std::string_view,14> Eyes{"Neutral","Sad","Angry","Confused","Laughing","Shocked","Happy",
    "Yawning","Sleeping","LookUp","LookDown","LookLeft","LookRight","Blink"};
constexpr std::array<std::string_view,5> Eyebrows{"Neutral","Sad","Angry","Confused","Raised"};
constexpr std::array<std::string_view,14> Mouths{"Neutral","Sad","Angry","Confused","Laughing","Shocked","Happy",
    "PhoneticO","PhoneticAi","PhoneticEe","PhoneticFv","PhoneticW","PhoneticL","PhoneticDth"};
// The clients' bounds (vertices are addressed with 16-bit indices).
constexpr std::size_t MaximumPrimitives=64, MaximumVertices=65535, MaximumTotalVertices=200000, MaximumIndices=3*120000;
constexpr std::size_t MaximumImageBytes=1u<<20, MaximumAnimations=64, MaximumKeys=4096, MaximumJson=4u<<20;
constexpr float MaximumCoordinate=5.0f;

[[noreturn]] void invalid(){throw Error("INVALID_ARGUMENT");}
void require(bool condition){if(!condition)invalid();}

std::uint32_t le32(std::string_view b,std::size_t at)
{
    std::uint32_t v=0;
    for(int i=3;i>=0;--i)v=(v<<8)|static_cast<unsigned char>(b[at+static_cast<std::size_t>(i)]);
    return v;
}
std::uint32_t be32(std::string_view b,std::size_t at)
{
    std::uint32_t v=0;
    for(std::size_t i=0;i<4;++i)v=(v<<8)|static_cast<unsigned char>(b[at+i]);
    return v;
}

std::size_t index(const Json& object,const char* key,std::size_t count)
{
    require(object.contains(key)&&object[key].is_number_unsigned());
    const auto value=object[key].get<std::size_t>();
    require(value<count);
    return value;
}

bool finite(float v){return std::isfinite(v)&&std::fabs(v)<=MaximumCoordinate*20;}

struct Glb {
    Json json;
    std::string_view bin;

    const Json& array(const char* key) const
    {
        static const Json empty=Json::array();
        if(!json.contains(key))return empty;
        require(json[key].is_array());
        return json[key];
    }

    // Floats of every element of an accessor, normalized integers scaled as glTF defines.
    std::vector<float> floats(std::size_t accessorIndex,std::string_view type,std::size_t maximumCount,std::size_t& count) const
    {
        const auto& accessors=array("accessors");
        require(accessorIndex<accessors.size());
        const auto& a=accessors[accessorIndex];
        require(a.is_object()&&!a.contains("sparse")&&a.value("type",std::string())==type);
        count=a.value("count",std::size_t{0});
        require(count>0&&count<=maximumCount);
        const int component=a.value("componentType",0);
        const bool normalized=a.value("normalized",false);
        const std::size_t width=type=="SCALAR"?1:type=="VEC2"?2:type=="VEC3"?3:type=="VEC4"?4:type=="MAT4"?16:0;
        require(width>0);
        std::size_t size=0;
        switch(component) {
        case 5126: size=4;require(!normalized);break;
        case 5120: case 5121: size=1;break;
        case 5122: case 5123: size=2;break;
        case 5125: size=4;require(!normalized);break;
        default: invalid();
        }
        const auto& views=array("bufferViews");
        const auto view=index(a,"bufferView",views.size());
        const auto& v=views[view];
        const auto viewOffset=v.value("byteOffset",std::size_t{0}), viewLength=v.value("byteLength",std::size_t{0});
        require(v.value("buffer",1)==0&&viewOffset<=bin.size()&&viewLength<=bin.size()-viewOffset);
        const auto element=size*width;
        const auto stride=v.contains("byteStride")?v["byteStride"].get<std::size_t>():element;
        require(stride>=element&&stride<=252&&(!v.contains("byteStride")||stride%4==0));
        const auto offset=a.value("byteOffset",std::size_t{0});
        require(offset<=viewLength&&stride*(count-1)+element<=viewLength-offset);
        std::vector<float> out(count*width);
        const char* base=bin.data()+viewOffset+offset;
        for(std::size_t i=0;i<count;++i)
            for(std::size_t k=0;k<width;++k) {
                const char* p=base+i*stride+k*size;
                float value=0;
                switch(component) {
                case 5126: std::memcpy(&value,p,4);break;
                case 5120: {std::int8_t x;std::memcpy(&x,p,1);value=normalized?std::max(-1.0f,x/127.0f):x;break;}
                case 5121: {std::uint8_t x;std::memcpy(&x,p,1);value=normalized?x/255.0f:x;break;}
                case 5122: {std::int16_t x;std::memcpy(&x,p,2);value=normalized?std::max(-1.0f,x/32767.0f):x;break;}
                case 5123: {std::uint16_t x;std::memcpy(&x,p,2);value=normalized?x/65535.0f:x;break;}
                case 5125: {std::uint32_t x;std::memcpy(&x,p,4);value=static_cast<float>(x);break;}
                default: break;
                }
                require(component==5126?finite(value):std::isfinite(value));
                out[i*width+k]=value;
            }
        return out;
    }
};

Glb open(std::string_view bytes)
{
    require(bytes.size()>=20&&bytes.substr(0,4)=="glTF"&&le32(bytes,4)==2&&le32(bytes,8)==bytes.size());
    const auto jsonLength=le32(bytes,12);
    require(le32(bytes,16)==0x4E4F534A&&jsonLength<=MaximumJson&&jsonLength<=bytes.size()-20);
    Glb glb;
    glb.json=Json::parse(bytes.substr(20,jsonLength),nullptr,false);
    require(!glb.json.is_discarded()&&glb.json.is_object());
    const auto binAt=20+static_cast<std::size_t>(jsonLength);
    require(binAt+8<=bytes.size());
    const auto binLength=le32(bytes,binAt);
    require(le32(bytes,binAt+4)==0x004E4942&&binLength<=bytes.size()-binAt-8);
    glb.bin=bytes.substr(binAt+8,binLength);
    // Everything lives in the file: one buffer, the binary chunk, no URIs or required extensions.
    require(!glb.json.contains("extensionsRequired"));
    const auto& buffers=glb.array("buffers");
    require(buffers.size()==1&&!buffers[0].contains("uri")&&buffers[0].value("byteLength",std::size_t{0})<=glb.bin.size());
    for(const auto& image:glb.array("images"))require(image.is_object()&&!image.contains("uri"));
    return glb;
}

void checkPng(std::string_view png)
{
    require(png.size()>=24&&png.substr(0,8)==std::string_view("\x89PNG\r\n\x1a\n",8)&&png.substr(12,4)=="IHDR");
    require(be32(png,16)>=1&&be32(png,16)<=2048&&be32(png,20)>=1&&be32(png,20)<=2048);
}

void checkMaterial(const Glb& glb,std::size_t materialIndex)
{
    const auto& materials=glb.array("materials");
    require(materialIndex<materials.size());
    const auto& m=materials[materialIndex];
    const auto extras=m.value("extras",Json::object());
    require(extras.is_object());
    const auto tint=extras.value("cnaTint",std::string("none"));
    require(std::ranges::find(Tints,tint)!=Tints.end());
    if(extras.contains("cnaFeature"))require(std::ranges::find(Features,extras["cnaFeature"].get<std::string>())!=Features.end());
    const int layer=extras.value("cnaLayer",0);
    require(layer==0||layer==1);
    const auto pbr=m.value("pbrMetallicRoughness",Json::object());
    if(pbr.contains("baseColorFactor")) {
        const auto& factor=pbr["baseColorFactor"];
        require(factor.is_array()&&factor.size()==4);
        for(const auto& c:factor)require(c.is_number()&&std::isfinite(c.get<float>()));
    }
    if(pbr.contains("baseColorTexture")) {
        const auto& textures=glb.array("textures");
        const auto texture=index(pbr["baseColorTexture"],"index",textures.size());
        const auto& images=glb.array("images");
        const auto image=index(textures[texture],"source",images.size());
        require(images[image].value("mimeType",std::string())=="image/png");
        const auto& views=glb.array("bufferViews");
        const auto& view=views[index(images[image],"bufferView",views.size())];
        const auto offset=view.value("byteOffset",std::size_t{0}), length=view.value("byteLength",std::size_t{0});
        require(length<=MaximumImageBytes&&offset<=glb.bin.size()&&length<=glb.bin.size()-offset);
        checkPng(glb.bin.substr(offset,length));
    }
}
}

AvatarGlbSummary validateAvatarGlb(std::string_view bytes)
{
    try {
        auto glb=open(bytes);
        AvatarGlbSummary summary;
        const auto& nodes=glb.array("nodes");
        const auto& skins=glb.array("skins");
        require(skins.size()==1&&skins[0].contains("joints")&&skins[0]["joints"].is_array()&&
                skins[0]["joints"].size()==AvatarJointCount);
        // Joints: named by rig slot, each exactly once, in XNA's parent topology, bind = translation.
        std::map<std::size_t,int> slotOfNode;
        std::vector<int> slotOfJoint;
        for(const auto& joint:skins[0]["joints"]) {
            require(joint.is_number_unsigned()&&joint.get<std::size_t>()<nodes.size());
            const auto node=joint.get<std::size_t>();
            const auto name=nodes[node].value("name",std::string());
            const auto found=std::ranges::find(Names,name);
            require(found!=Names.end()&&!slotOfNode.contains(node));
            const int slot=static_cast<int>(found-Names.begin());
            for(const auto& [other,otherSlot]:slotOfNode)require(otherSlot!=slot);
            slotOfNode[node]=slot;
            slotOfJoint.push_back(slot);
        }
        std::vector<int> parentSlot(AvatarJointCount,-1);
        for(const auto& [node,slot]:slotOfNode) {
            const auto& n=nodes[node];
            require(!n.contains("matrix")&&!n.contains("mesh"));
            if(n.contains("scale"))for(const auto& s:n["scale"])require(s.is_number()&&s.get<float>()==1.0f);
            if(n.contains("rotation")) {
                require(n["rotation"].is_array()&&n["rotation"].size()==4);
                require(std::fabs(n["rotation"][3].get<float>())>=0.99999f);
            }
            if(n.contains("translation")) {
                require(n["translation"].is_array()&&n["translation"].size()==3);
                for(std::size_t k=0;k<3;++k) {
                    const auto v=n["translation"][k].get<float>();
                    require(std::isfinite(v)&&std::fabs(v)<MaximumCoordinate);
                    summary.bind[static_cast<std::size_t>(slot)][k]=v;
                }
            }
            if(n.contains("children"))
                for(const auto& child:n["children"]) {
                    require(child.is_number_unsigned());
                    auto found=slotOfNode.find(child.get<std::size_t>());
                    if(found!=slotOfNode.end())parentSlot[static_cast<std::size_t>(found->second)]=slot;
                }
        }
        for(std::size_t slot=0;slot<AvatarJointCount;++slot)require(parentSlot[slot]==Parents[slot]);
        // Meshes: skinned triangle lists within the clients' bounds.
        std::size_t total=0;
        const auto& meshes=glb.array("meshes");
        for(const auto& node:nodes) {
            if(!node.contains("mesh"))continue;
            const auto mesh=index(node,"mesh",meshes.size());
            require(node.contains("skin")&&node["skin"]==0);
            const auto& primitives=meshes[mesh].at("primitives");
            require(primitives.is_array());
            for(const auto& p:primitives) {
                require(++summary.primitives<=MaximumPrimitives);
                require(p.value("mode",4)==4&&!p.contains("targets")&&p.contains("indices")&&p.contains("attributes"));
                const auto& attributes=p["attributes"];
                require(attributes.is_object());
                for(const auto& [name,value]:attributes.items())
                    require(name=="POSITION"||name=="NORMAL"||name=="TEXCOORD_0"||name=="JOINTS_0"||name=="WEIGHTS_0");
                require(attributes.contains("POSITION")&&attributes.contains("NORMAL")&&attributes.contains("JOINTS_0")&&
                        attributes.contains("WEIGHTS_0"));
                std::size_t count=0, other=0;
                const auto positions=glb.floats(attributes["POSITION"].get<std::size_t>(),"VEC3",MaximumVertices,count);
                require(std::ranges::all_of(positions,[](float v){return std::fabs(v)<=MaximumCoordinate;}));
                require((total+=count)<=MaximumTotalVertices);
                (void)glb.floats(attributes["NORMAL"].get<std::size_t>(),"VEC3",MaximumVertices,other);
                require(other==count);
                if(attributes.contains("TEXCOORD_0")) {
                    (void)glb.floats(attributes["TEXCOORD_0"].get<std::size_t>(),"VEC2",MaximumVertices,other);
                    require(other==count);
                }
                const auto joints=glb.floats(attributes["JOINTS_0"].get<std::size_t>(),"VEC4",MaximumVertices,other);
                require(other==count);
                const auto weights=glb.floats(attributes["WEIGHTS_0"].get<std::size_t>(),"VEC4",MaximumVertices,other);
                require(other==count);
                for(std::size_t v=0;v<count;++v) {
                    float sum=0;
                    for(std::size_t k=0;k<4;++k) {
                        const float j=joints[v*4+k], w=weights[v*4+k];
                        require(j>=0&&j<static_cast<float>(AvatarJointCount)&&j==std::floor(j)&&w>=0);
                        sum+=w;
                    }
                    require(sum>=0.5f);
                }
                const auto& accessors=glb.array("accessors");
                const auto indexAccessor=index(p,"indices",accessors.size());
                const int indexType=accessors[indexAccessor].value("componentType",0);
                require((indexType==5121||indexType==5123||indexType==5125)&&!accessors[indexAccessor].value("normalized",false));
                std::size_t indexCount=0;
                const auto indices=glb.floats(indexAccessor,"SCALAR",MaximumIndices,indexCount);
                require(indexCount%3==0&&std::ranges::all_of(indices,[&](float i){return i<static_cast<float>(count);}));
                require(p.contains("material"));
                checkMaterial(glb,p["material"].get<std::size_t>());
            }
        }
        // Animations: cubic-spline joint curves with keyed expressions.
        const auto& animations=glb.array("animations");
        require(animations.size()<=MaximumAnimations);
        for(const auto& animation:animations) {
            ++summary.animations;
            const auto& samplers=animation.at("samplers");
            for(const auto& channel:animation.at("channels")) {
                const auto& target=channel.at("target");
                auto found=slotOfNode.find(index(target,"node",nodes.size()));
                require(found!=slotOfNode.end());
                const auto path=target.value("path",std::string());
                require(path=="rotation"||(path=="translation"&&found->second==0));
                require(samplers.is_array());
                const auto& sampler=samplers.at(index(channel,"sampler",samplers.size()));
                require(sampler.value("interpolation",std::string())=="CUBICSPLINE");
                std::size_t keys=0, values=0;
                const auto times=glb.floats(index(sampler,"input",glb.array("accessors").size()),"SCALAR",MaximumKeys,keys);
                require(times.front()>=0&&std::ranges::is_sorted(times));
                (void)glb.floats(index(sampler,"output",glb.array("accessors").size()),path=="rotation"?"VEC4":"VEC3",
                                 MaximumKeys*3,values);
                require(values==keys*3);
            }
            const auto extras=animation.value("extras",Json::object());
            if(extras.contains("cnaExpressions")) {
                const auto& keys=extras["cnaExpressions"];
                require(keys.is_array()&&keys.size()<=MaximumKeys);
                double last=-1;
                for(const auto& key:keys) {
                    require(key.is_array()&&key.size()==6&&key[0].is_number());
                    const double time=key[0].get<double>();
                    require(std::isfinite(time)&&time>=last);
                    last=time;
                    const std::array<int,5> limits{13,13,13,4,4};
                    for(std::size_t k=1;k<6;++k)require(key[k].is_number_integer()&&key[k].get<int>()>=0&&key[k].get<int>()<=limits[k-1]);
                }
            }
        }
        return summary;
    } catch(const Json::exception&) {
        invalid();
    }
}

void validateFaceAtlas(std::string_view png,const Json& layout)
{
    try {
        checkPng(png);
        const int tile=layout.at("tileSize").get<int>(), columns=layout.at("columns").get<int>();
        const int width=layout.at("width").get<int>(), height=layout.at("height").get<int>();
        require(tile>=8&&tile<=256&&columns>=1&&width==columns*tile&&height>=tile&&height<=4096&&height%tile==0);
        require(static_cast<int>(be32(png,16))==width&&static_cast<int>(be32(png,20))==height);
        const int tiles=columns*(height/tile);
        auto inRange=[&](const Json& value){require(value.is_number_integer()&&value.get<int>()>=0&&value.get<int>()<tiles);};
        for(auto state:Eyes)
            for(const auto* side:{"left","right"}) {
                const auto& pair=layout.at("eyes").at(std::string(state)).at(side);
                require(pair.is_array()&&pair.size()==2);
                inRange(pair[0]);
                inRange(pair[1]);
            }
        for(auto state:Eyebrows)
            for(const auto* side:{"left","right"})inRange(layout.at("eyebrows").at(std::string(state)).at(side));
        for(auto state:Mouths)inRange(layout.at("mouths").at(std::string(state)));
    } catch(const Json::exception&) {
        invalid();
    }
}

void validateFaceControls(const Json& controls)
{
    try {
        require(controls.is_object());
        auto vector3=[](const Json& value,float bound) {
            require(value.is_array()&&value.size()==3);
            std::array<float,3> v{};
            for(std::size_t k=0;k<3;++k) {
                v[k]=value[k].get<float>();
                require(std::isfinite(v[k])&&std::fabs(v[k])<=bound);
            }
            return v;
        };
        for(const auto* body:{"female","male"}) {
            const auto& list=controls.at(body);
            require(list.is_array()&&list.size()<=32);
            for(const auto& control:list) {
                const int parameter=control.at("parameter").get<int>();
                require(parameter>=0&&parameter<16);
                const auto scope=control.at("scope").get<std::string>();
                require(scope=="face"||scope=="head");
                const auto& ops=control.at("ops");
                require(ops.is_array()&&ops.size()<=8);
                for(const auto& op:ops) {
                    (void)vector3(op.at("centre"),5.0f);
                    const auto radii=vector3(op.at("radii"),1.0f);
                    require(radii[0]>=1e-3f&&radii[1]>=1e-3f&&radii[2]>=1e-3f);
                    const float inner=op.at("inner").get<float>();
                    require(inner>=0.0f&&inner<1.0f);
                    if(op.contains("scale")) {
                        const auto s=vector3(op["scale"],2.0f);
                        require(s[0]>=0.5f&&s[1]>=0.5f&&s[2]>=0.5f);
                    } else if(op.contains("move")) {
                        (void)vector3(op["move"],0.1f);
                    } else {
                        const auto axis=vector3(op.at("rotate"),1.0f);
                        require(std::sqrt(axis[0]*axis[0]+axis[1]*axis[1]+axis[2]*axis[2])>=1e-3f);
                        require(std::fabs(op.at("degrees").get<float>())<=45.0f);
                    }
                }
            }
        }
    } catch(const Json::exception&) {
        invalid();
    }
}
}
