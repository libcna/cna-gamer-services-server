// SPDX-License-Identifier: MIT
#include "CnaService/Avatars.hpp"
#include "CnaService/AvatarAssets.hpp"
#include <algorithm>
#include <cmath>
#include <array>
#include <random>
#include <set>
#include <vector>

namespace CnaService {
namespace {
// CNA description layout (CNA docs/avatars.md). Format 1: version, "CNA", body, height mm, build,
// catalog version, 7 RGB colors, 6 item ids, zero reserved bytes, CRC-32 of everything before it.
// Format 2 keeps bytes 1-42 and adds a facial-hair feature item id and 16 face-shape bytes. Format 3
// keeps format 2 and adds eyebrows, ears, four accessories (id and side), four makeup styles, five
// more colours, proportions and 16 more face bytes.
constexpr std::size_t HeightOffset=5, BuildOffset=7, CatalogOffset=8, ColorOffset=10, ItemOffset=31, FacialHairOffset=43,
    FaceOffset=45, EyebrowsOffset=61, EarsOffset=63, AccessoryOffset=65, MakeupOffset=77, ExtendedColorOffset=85,
    ProportionOffset=100, ExtendedFaceOffset=101, ChecksumOffset=AvatarDescriptionSize-4;
// Where each format's reserved (zero) bytes begin.
constexpr std::array<std::size_t,4> ReservedOffset{0,43,61,117};
constexpr int Neutral=128;
// Kinds by slot; slot numbers are stored in avatar_catalog_items.
constexpr std::array<std::string_view,17> Slots{"hair","top","bottom","shoes","glasses","hat","facialHair","ears",
    "earrings","wristwear","gloves","rings","eyebrows","eyeShadow","eyeliner","blush","lips"};
constexpr int GlassesSlot=4, HatSlot=5, FacialHairSlot=6, EarsSlot=7, EarringsSlot=8, GlovesSlot=10, EyebrowsSlot=12,
    EyeShadowSlot=13, EyelinerSlot=14, LipsSlot=16;
enum class List : std::uint8_t { Items, Features, Styles };
List listOf(int slot){return slot==FacialHairSlot||slot==EarsSlot?List::Features:slot>=EyebrowsSlot?List::Styles:List::Items;}
// Atlas tiles a face style names: eyebrows one per state and side, lips one per mouth state,
// eyeliner one per side for an open and a closed eye, the other makeup one per side.
std::size_t styleTiles(int slot){return slot==EyebrowsSlot?10:slot==LipsSlot?14:slot==EyelinerSlot?4:2;}
constexpr std::array<std::string_view,12> ColorNames{"skin","hair","eyes","top","bottom","shoes","accessory","lips",
    "eyeShadow","blush","jewelry","gloves"};
constexpr std::size_t MaximumAssetBytes=8u<<20, MaximumAssets=1024, MaximumEntries=1024;

unsigned read16(std::string_view bytes,std::size_t at)
{
    return static_cast<unsigned char>(bytes[at])|static_cast<unsigned>(static_cast<unsigned char>(bytes[at+1]))<<8;
}

bool safeName(std::string_view name)
{
    return !name.empty()&&name.size()<=64&&name.front()!='.'&&
        std::ranges::all_of(name,[](char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='.'||c=='-';});
}

void checkFile(std::string_view name,std::string_view bytes)
{
    auto big32=[&](std::size_t at){unsigned v=0;for(std::size_t i=at;i<at+4;++i)v=(v<<8)|static_cast<unsigned char>(bytes[i]);return v;};
    auto little32=[&](std::size_t at){unsigned v=0;for(int i=3;i>=0;--i)v=(v<<8)|static_cast<unsigned char>(bytes[at+i]);return v;};
    if(name.ends_with(".glb")) {
        if(bytes.size()<20||bytes.substr(0,4)!="glTF"||little32(4)!=2||little32(8)!=bytes.size())throw Error("INVALID_ARGUMENT");
    } else if(name.ends_with(".png")) {
        if(bytes.size()<24||bytes.substr(0,8)!=std::string_view("\x89PNG\r\n\x1a\n",8)||bytes.substr(12,4)!="IHDR"||
           big32(16)<1||big32(16)>2048||big32(20)<1||big32(20)>2048)throw Error("INVALID_ARGUMENT");
    } else throw Error("INVALID_ARGUMENT");
}

// Everything a description says, whatever its format; what a format cannot say is neutral.
using Rgb=std::array<unsigned char,3>;
struct Fields {
    int body=0, height=0, build=0;
    long long catalog=0;
    std::array<Rgb,12> colors{};
    std::array<long long,6> items{};
    long long facialHair=0, eyebrows=0, ears=0;
    std::array<long long,4> accessories{}, makeup{};
    std::array<int,4> sides{};
    int proportion=Neutral;
    std::array<int,32> face=[]{std::array<int,32> neutral{};neutral.fill(Neutral);return neutral;}();
};

Fields read(std::string_view d)
{
    Fields f;
    const int format=static_cast<unsigned char>(d[0]);
    auto byte=[&](std::size_t at){return static_cast<int>(static_cast<unsigned char>(d[at]));};
    auto rgb=[&](std::size_t at){return Rgb{static_cast<unsigned char>(d[at]),static_cast<unsigned char>(d[at+1]),static_cast<unsigned char>(d[at+2])};};
    f.body=byte(4);f.height=static_cast<int>(read16(d,HeightOffset));f.build=byte(BuildOffset);f.catalog=read16(d,CatalogOffset);
    for(std::size_t c=0;c<7;++c)f.colors[c]=rgb(ColorOffset+c*3);
    for(std::size_t slot=0;slot<6;++slot)f.items[slot]=read16(d,ItemOffset+slot*2);
    if(format>=2) {
        f.facialHair=read16(d,FacialHairOffset);
        for(std::size_t k=0;k<16;++k)f.face[k]=byte(FaceOffset+k);
    }
    if(format>=3) {
        f.eyebrows=read16(d,EyebrowsOffset);f.ears=read16(d,EarsOffset);
        for(std::size_t i=0;i<4;++i) {
            f.accessories[i]=read16(d,AccessoryOffset+i*3);
            f.sides[i]=byte(AccessoryOffset+i*3+2);
            f.makeup[i]=read16(d,MakeupOffset+i*2);
        }
        for(std::size_t c=0;c<5;++c)f.colors[7+c]=rgb(ExtendedColorOffset+c*3);
        f.proportion=byte(ProportionOffset);
        for(std::size_t k=0;k<16;++k)f.face[16+k]=byte(ExtendedFaceOffset+k);
    }
    return f;
}

bool usesFace(const Fields& f)
{
    return f.facialHair!=0||std::any_of(f.face.begin(),f.face.begin()+16,[](int v){return v!=Neutral;});
}

bool usesExtended(const Fields& f)
{
    auto set=[](const auto& values){return std::ranges::any_of(values,[](auto v){return v!=decltype(v){};});};
    return f.eyebrows!=0||f.ears!=0||set(f.accessories)||set(f.sides)||set(f.makeup)||f.proportion!=Neutral||
        std::any_of(f.face.begin()+16,f.face.end(),[](int v){return v!=Neutral;})||
        std::any_of(f.colors.begin()+7,f.colors.end(),[](const Rgb& c){return c!=Rgb{};});
}

void setChecksum(std::string& d)
{
    const auto crc=crc32(std::string_view(d).substr(0,ChecksumOffset));
    for(int i=0;i<4;++i)d[ChecksumOffset+i]=static_cast<char>((crc>>(8*i))&0xff);
}

std::string write(const Fields& f,int format)
{
    std::string d(AvatarDescriptionSize,'\0');
    auto put16=[&](std::size_t at,long long value){d[at]=static_cast<char>(value&0xff);d[at+1]=static_cast<char>((value>>8)&0xff);};
    auto putRgb=[&](std::size_t at,const Rgb& c){for(std::size_t k=0;k<3;++k)d[at+k]=static_cast<char>(c[k]);};
    d[0]=static_cast<char>(format);d[1]='C';d[2]='N';d[3]='A';
    d[4]=static_cast<char>(f.body);put16(HeightOffset,f.height);d[BuildOffset]=static_cast<char>(f.build);put16(CatalogOffset,f.catalog);
    for(std::size_t c=0;c<7;++c)putRgb(ColorOffset+c*3,f.colors[c]);
    for(std::size_t slot=0;slot<6;++slot)put16(ItemOffset+slot*2,f.items[slot]);
    if(format>=2) {
        put16(FacialHairOffset,f.facialHair);
        for(std::size_t k=0;k<16;++k)d[FaceOffset+k]=static_cast<char>(f.face[k]);
    }
    if(format>=3) {
        put16(EyebrowsOffset,f.eyebrows);put16(EarsOffset,f.ears);
        for(std::size_t i=0;i<4;++i) {
            put16(AccessoryOffset+i*3,f.accessories[i]);
            d[AccessoryOffset+i*3+2]=static_cast<char>(f.sides[i]);
            put16(MakeupOffset+i*2,f.makeup[i]);
        }
        for(std::size_t c=0;c<5;++c)putRgb(ExtendedColorOffset+c*3,f.colors[7+c]);
        d[ProportionOffset]=static_cast<char>(f.proportion);
        for(std::size_t k=0;k<16;++k)d[ExtendedFaceOffset+k]=static_cast<char>(f.face[16+k]);
    }
    setChecksum(d);
    return d;
}

// The lowest format that says everything a description says: its fields and the formats of the
// entries it names (formatOf gives an entry's format, 0 for none).
template<typename FormatOf>
int lowestFormat(const Fields& f,FormatOf&& formatOf)
{
    int format=usesExtended(f)?3:usesFace(f)?2:1;
    for(auto id:f.items)if(id)format=std::max(format,formatOf(id));
    if(f.facialHair)format=std::max(format,formatOf(f.facialHair));
    return format;
}

// Palettes the administration generator draws from; a description may carry any RGB value.
constexpr std::array<Rgb,8> SkinTones{{{255,224,196},{241,194,160},{224,172,128},{198,134,90},{160,104,68},{120,78,52},{92,58,40},{255,210,180}}};
constexpr std::array<Rgb,8> HairColors{{{36,28,24},{74,48,30},{120,78,40},{176,122,62},{226,188,116},{160,60,36},{200,200,196},{60,64,120}}};
constexpr std::array<Rgb,6> EyeColors{{{70,46,30},{110,72,40},{60,110,160},{70,130,90},{120,120,130},{40,40,44}}};
constexpr std::array<Rgb,12> ClothColors{{{220,60,56},{240,150,40},{250,210,60},{90,180,80},{50,150,200},{60,80,180},
    {130,80,170},{230,120,170},{240,240,236},{60,62,70},{120,90,60},{40,110,110}}};
}

std::uint32_t crc32(std::string_view bytes)
{
    std::uint32_t crc=0xffffffffu;
    for(unsigned char byte:bytes) {
        crc^=byte;
        for(int bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));
    }
    return ~crc;
}

Json parseAvatarManifest(std::string_view text)
{
    if(text.empty()||text.size()>MaximumManifestBytes)throw Error("LIMIT_EXCEEDED");
    std::vector<std::pair<bool,std::size_t>> frames;
    std::vector<std::set<std::string>> keys;
    try {
        return Json::parse(text,[&](int depth,Json::parse_event_t event,Json& value) {
            using Event=Json::parse_event_t;
            if(depth>16)throw Error("LIMIT_EXCEEDED");
            if((event==Event::object_start||event==Event::array_start||event==Event::value)&&!frames.empty()&&frames.back().first&&
               ++frames.back().second>4096)throw Error("LIMIT_EXCEEDED");
            if(event==Event::object_start||event==Event::array_start) {
                frames.emplace_back(event==Event::array_start,0);
                keys.emplace_back();
            }
            if(event==Event::key) {
                if(!keys.back().insert(value.get<std::string>()).second)throw Error("MALFORMED_MESSAGE");
                if(keys.back().size()>256)throw Error("LIMIT_EXCEEDED");
            }
            if(event==Event::object_end||event==Event::array_end){frames.pop_back();keys.pop_back();}
            return true;
        });
    } catch(const Error&) {
        throw;
    } catch(const Json::exception&) {
        throw Error("MALFORMED_MESSAGE");
    }
}

void validateAvatarDescription(sqlite3* db,std::string_view d)
{
    if(d.size()!=AvatarDescriptionSize||d[0]<1||d[0]>NewestDescriptionFormat||d.substr(1,3)!="CNA"||static_cast<unsigned char>(d[4])>1)
        throw Error("INVALID_ARGUMENT");
    const int format=d[0];
    const unsigned height=read16(d,HeightOffset);
    if(height<1450||height>2050)throw Error("INVALID_ARGUMENT");
    if(std::any_of(d.begin()+static_cast<std::ptrdiff_t>(ReservedOffset[static_cast<std::size_t>(format)]),d.begin()+ChecksumOffset,
                   [](char c){return c!=0;}))
        throw Error("INVALID_ARGUMENT");
    std::uint32_t stored=0;
    for(int i=3;i>=0;--i)stored=(stored<<8)|static_cast<unsigned char>(d[ChecksumOffset+i]);
    if(stored!=crc32(d.substr(0,ChecksumOffset)))throw Error("INVALID_ARGUMENT");
    // A development catalog below the baseline is refused even before the catalog table is asked.
    const long long version=read16(d,CatalogOffset);
    if(!supportedCatalogVersion(version))throw Error("INVALID_ARGUMENT");
    Statement catalog(db,"SELECT 1 FROM avatar_catalogs WHERE version=?");catalog.bind(1,version);
    if(!catalog.row())throw Error("INVALID_ARGUMENT");
    const auto f=read(d);
    // Every id names an entry of that catalog in its slot.
    std::map<long long,int> formats;
    auto named=[&](long long id,int slot) {
        Statement item(db,"SELECT slot,format FROM avatar_catalog_items WHERE version=? AND id=?");item.bind(1,version);item.bind(2,id);
        if(!item.row()||item.number(0)!=slot)throw Error("INVALID_ARGUMENT");
        formats[id]=static_cast<int>(item.number(1));
    };
    for(int slot=0;slot<6;++slot) {
        const auto id=f.items[static_cast<std::size_t>(slot)];
        if(id==0) {if(slot!=GlassesSlot&&slot!=HatSlot)throw Error("INVALID_ARGUMENT");continue;}
        named(id,slot);
    }
    if(f.facialHair)named(f.facialHair,FacialHairSlot);
    if(f.eyebrows)named(f.eyebrows,EyebrowsSlot);
    if(f.ears)named(f.ears,EarsSlot);
    for(std::size_t i=0;i<4;++i) {
        // A side belongs to a worn accessory, and gloves are always a pair.
        const int slot=EarringsSlot+static_cast<int>(i);
        if(f.sides[i]>2||(f.sides[i]!=0&&(f.accessories[i]==0||slot==GlovesSlot)))throw Error("INVALID_ARGUMENT");
        if(f.accessories[i])named(f.accessories[i],slot);
        if(f.makeup[i])named(f.makeup[i],EyeShadowSlot+static_cast<int>(i));
    }
    // One canonical encoding per avatar: exactly the lowest format that says all of it.
    if(lowestFormat(f,[&](long long id){return formats.at(id);})!=format)throw Error("INVALID_ARGUMENT");
}

namespace {
int storeCatalog(Store& store,const Json& manifest,long long version,long long revision,const std::map<std::string,std::string>& hashes,
    const std::map<std::string,std::string>& files,const std::map<long long,CatalogItemInfo>& items);

void require(bool condition){if(!condition)throw Error("INVALID_ARGUMENT");}

std::array<float,3> vector3(const Json& value,float bound)
{
    require(value.is_array()&&value.size()==3);
    std::array<float,3> v{};
    for(std::size_t k=0;k<3;++k) {
        v[k]=value[k].get<float>();
        require(std::isfinite(v[k])&&std::fabs(v[k])<=bound);
    }
    return v;
}

// What the assembly and the editor read beyond items: anchors, the head surface, outfits, presets.
void validateFaceData(const Json& manifest,const std::map<long long,CatalogItemInfo>& items)
{
    for(const auto* body:{"female","male"}) {
        if(manifest.contains("faceAnchors"))
            for(const auto* anchor:{"eyeLeft","eyeRight","earLeft","earRight"})(void)vector3(manifest["faceAnchors"].at(body).at(anchor),5.0f);
        if(manifest.contains("headSurface")) {
            const auto& surface=manifest["headSurface"].at(body);
            (void)vector3(surface.at("centre"),5.0f);
            const int rings=surface.at("rings").get<int>(), segments=surface.at("segments").get<int>();
            require(rings>=2&&rings<=128&&segments>=4&&segments<=256);
            const auto& radii=surface.at("radii");
            require(radii.is_array()&&radii.size()==static_cast<std::size_t>((rings+1)*segments));
            for(const auto& radius:radii)require(radius.get<float>()>0.01f&&radius.get<float>()<1.0f);
        }
    }
    if(manifest.contains("outfits")) {
        require(manifest["outfits"].is_array()&&manifest["outfits"].size()<=256);
        for(const auto& outfit:manifest["outfits"]) {
            const auto name=outfit.at("name").get<std::string>();
            require(!name.empty()&&name.size()<=64);
            for(const auto& [kind,value]:outfit.at("items").items()) {
                const auto slot=std::ranges::find(Slots,kind);
                const auto id=value.get<long long>();
                require(slot!=Slots.end()&&id>=0&&id<=65535);
                if(id) {
                    const auto found=items.find(id);
                    require(found!=items.end()&&found->second.slot==slot-Slots.begin());
                }
            }
            if(outfit.contains("colors"))
                for(const auto& [name,rgb]:outfit["colors"].items()) {
                    require(std::ranges::find(ColorNames,name)!=ColorNames.end()&&rgb.is_array()&&rgb.size()==3);
                    for(const auto& channel:rgb)require(channel.get<int>()>=0&&channel.get<int>()<=255);
                }
        }
    }
    if(manifest.contains("facePresets")) {
        require(manifest["facePresets"].is_array()&&manifest["facePresets"].size()<=256);
        for(const auto& preset:manifest["facePresets"]) {
            const auto group=preset.at("group").get<std::string>(), name=preset.at("name").get<std::string>();
            require(!group.empty()&&group.size()<=16&&!name.empty()&&name.size()<=32);
            for(const auto& [control,value]:preset.at("values").items())
                require(control.size()<=32&&value.get<float>()>=-1.0f&&value.get<float>()<=1.0f);
        }
    }
}
}

int importAvatarCatalog(Store& store,std::string_view text,const std::map<std::string,std::string>& files)
{
    const auto manifest=parseAvatarManifest(text);
    if(!manifest.is_object()||manifest.value("format",0)!=1||manifest.value("rig",std::string())!="cna-avatar-71")
        throw Error("INVALID_ARGUMENT");
    // Catalogs 1 and 2 were CNA development catalogs: never imported.
    const auto version=integerField(manifest,"catalogVersion",BaseCatalogVersion,MaximumCatalogVersion);
    // The contract levels these checks enforce; a catalog for newer readers needs a newer service.
    if(manifest.contains("reader"))(void)integerField(manifest,"reader",1,2);
    const auto revision=manifest.contains("revision")?integerField(manifest,"revision",1,65535):1;
    if(!manifest.contains("assets")||!manifest["assets"].is_array()||manifest["assets"].empty()||manifest["assets"].size()>MaximumAssets)
        throw Error("INVALID_ARGUMENT");
    std::map<std::string,std::string> hashes;
    std::map<std::string,AvatarGlbSummary> models;
    for(const auto& asset:manifest["assets"]) {
        const auto name=stringField(asset,"name",64), hash=stringField(asset,"sha256",64);
        const auto size=integerField(asset,"size",1,MaximumAssetBytes);
        if(!safeName(name)||hash.size()!=64||hash.find_first_not_of("0123456789abcdef")!=std::string::npos||
           hashes.contains(name))throw Error("INVALID_ARGUMENT");
        auto file=files.find(name);
        if(file==files.end()||static_cast<long long>(file->second.size())!=size||sha256(file->second)!=hash)
            throw Error("INVALID_ARGUMENT");
        checkFile(name,file->second);
        // Every model is checked against the contract CNA clients enforce.
        if(name.ends_with(".glb"))models.emplace(name,validateAvatarGlb(file->second));
        hashes[name]=hash;
    }
    // A texture a model shares with others is a PNG of the catalog.
    for(const auto& [name,summary]:models)
        for(const auto& texture:summary.textures)require(texture.ends_with(".png")&&hashes.contains(texture));
    auto listed=[&](const Json& value)->const std::string& {
        if(!value.is_string()||!hashes.contains(value.get<std::string>()))throw Error("INVALID_ARGUMENT");
        return value.get_ref<const std::string&>();
    };
    auto model=[&](const Json& value)->const AvatarGlbSummary& {
        auto found=models.find(listed(value));
        if(found==models.end())throw Error("INVALID_ARGUMENT");
        return found->second;
    };
    try {
        std::array<const AvatarGlbSummary*,2> bodies{};
        for(int body=0;body<2;++body) {
            const auto& entry=manifest.at("bodies").at(body==0?"female":"male");
            bodies[body]=&model(entry.at("asset"));
            if(bodies[body]->primitives==0)throw Error("INVALID_ARGUMENT");
            const auto authored=integerField(entry,"authoredHeightMillimeters",1450,2050);
            (void)authored;
        }
        // An item a client cannot fit to its body makes the whole avatar unavailable there.
        auto fitted=[&](const Json& assets) {
            for(int body=0;body<2;++body) {
                const auto& item=model(assets.at(body==0?"female":"male"));
                if(item.primitives==0)throw Error("INVALID_ARGUMENT");
                for(std::size_t joint=0;joint<AvatarJointCount;++joint)
                    for(std::size_t k=0;k<3;++k)
                        if(std::fabs(item.bind[joint][k]-bodies[body]->bind[joint][k])>1e-3f)throw Error("INVALID_ARGUMENT");
            }
        };
        const auto& face=manifest.at("face");
        validateFaceAtlas(files.at(listed(face.at("asset"))),face.at("layout"));
        const int tiles=face["layout"].at("columns").get<int>()*(face["layout"].at("height").get<int>()/face["layout"].at("tileSize").get<int>());
        if(model(manifest.at("animations").at("asset")).animations!=31)throw Error("INVALID_ARGUMENT");
        if(manifest.contains("faceControls"))validateFaceControls(manifest["faceControls"]);
        std::map<long long,CatalogItemInfo> items;
        // Per required slot: an item every format can name (a projection's default), and one each
        // body type may draw at random.
        std::array<bool,4> anyFormat{};
        std::array<std::array<bool,2>,4> drawn{};
        auto readItems=[&](const char* key,List list) {
            if(!manifest.contains(key))return;
            if(!manifest[key].is_array()||manifest[key].size()>MaximumEntries)throw Error("INVALID_ARGUMENT");
            for(const auto& item:manifest[key]) {
                const auto id=integerField(item,"id",1,65535);
                const auto found=std::ranges::find(Slots,stringField(item,"slot",16));
                const int slot=static_cast<int>(found-Slots.begin());
                if(found==Slots.end()||items.contains(id)||listOf(slot)!=list)throw Error("INVALID_ARGUMENT");
                require(!stringField(item,"name",64).empty());
                if(item.contains("label"))(void)stringField(item,"label",40);
                // A kind format 3 introduced cannot be named by an older format.
                const auto format=item.contains("format")?integerField(item,"format",1,NewestDescriptionFormat):1;
                items[id]={slot,slot>=EarsSlot?3:static_cast<int>(format)};
                if(list==List::Styles) {
                    const auto& named=item.at("tiles");
                    require(!item.contains("assets")&&named.is_array()&&named.size()==styleTiles(slot));
                    for(const auto& tile:named)require(tile.is_number_integer()&&tile.get<int>()>=0&&tile.get<int>()<tiles);
                    if(item.contains("opacity"))require(item["opacity"].get<float>()>0.0f&&item["opacity"].get<float>()<=1.0f);
                } else {
                    fitted(item.at("assets"));
                }
                std::array<double,2> weights{1.0,1.0};
                if(item.contains("random"))
                    for(int body=0;body<2;++body) {
                        weights[static_cast<std::size_t>(body)]=item["random"].at(body==0?"female":"male").get<double>();
                        require(weights[static_cast<std::size_t>(body)]>=0&&weights[static_cast<std::size_t>(body)]<=1000);
                    }
                if(slot<4) {
                    anyFormat[static_cast<std::size_t>(slot)]=anyFormat[static_cast<std::size_t>(slot)]||format==1;
                    for(std::size_t body=0;body<2;++body)drawn[static_cast<std::size_t>(slot)][body]=drawn[static_cast<std::size_t>(slot)][body]||weights[body]>0;
                }
                // Earlier contract fields: hair variants under a hat, and a hat that covers hair.
                if(item.contains("hatAssets")) {
                    require(slot==0);
                    fitted(item["hatAssets"]);
                }
                require(!item.contains("coversHair")||(slot==HatSlot&&item["coversHair"].is_boolean()));
                if(item.contains("hairCover")) {
                    // Where a hat's brim meets the head, and how far hair under it is lifted.
                    require(slot==HatSlot);
                    const auto& line=item["hairCover"].at("line");
                    require(line.is_array()&&line.size()==4);
                    for(const auto& value:line)require(value.get<float>()>0.0f&&value.get<float>()<1.0f);
                    const float lift=item["hairCover"].at("lift").get<float>();
                    require(lift>=0.0f&&lift<=0.1f);
                }
                if(item.contains("follow")) {
                    const auto follow=item["follow"].get<std::string>();
                    require((follow=="eyes"&&slot==GlassesSlot)||(follow=="ears"&&slot==EarringsSlot));
                }
                if(item.contains("hides")) {
                    require(item["hides"].is_array()&&item["hides"].size()<=Slots.size());
                    for(const auto& hidden:item["hides"]) {
                        const auto kind=std::ranges::find(Slots,hidden.get<std::string>());
                        require(kind!=Slots.end()&&listOf(static_cast<int>(kind-Slots.begin()))==List::Items);
                    }
                }
            }
        };
        if(!manifest.contains("items"))throw Error("INVALID_ARGUMENT");
        readItems("items",List::Items);
        readItems("featureItems",List::Features);
        readItems("faceStyles",List::Styles);
        for(std::size_t required=0;required<4;++required)
            require(anyFormat[required]&&drawn[required][0]&&drawn[required][1]);
        validateFaceData(manifest,items);
        return storeCatalog(store,manifest,version,revision,hashes,files,items);
    } catch(const Json::exception&) {
        throw Error("INVALID_ARGUMENT");
    }
}

namespace {
int storeCatalog(Store& store,const Json& manifest,long long version,long long revision,const std::map<std::string,std::string>& hashes,
    const std::map<std::string,std::string>& files,const std::map<long long,CatalogItemInfo>& items)
{
    const auto canonical=manifest.dump();
    {
        Store::Transaction transaction(store);
        bool revise=false;
        {
            Statement existing(store.db(),"SELECT manifest,revision FROM avatar_catalogs WHERE version=?");existing.bind(1,version);
            if(existing.row()) {
                if(existing.text(0)==canonical)return static_cast<int>(version);
                // A version changes only to a higher revision.
                if(revision<=existing.number(1))throw Error("CONFLICT");
                revise=true;
            }
        }
        Statement newer(store.db(),"SELECT 1 FROM avatar_catalogs WHERE version>?");newer.bind(1,version);
        if(newer.row())throw Error(revise?"CONFLICT":"INVALID_ARGUMENT");
        if(revise) {
            // A revision of the newest catalog keeps every entry it had: its id, slot and the lowest
            // format that names it, so every stored description stays valid and canonical.
            Statement kept(store.db(),"SELECT id,slot,format FROM avatar_catalog_items WHERE version=?");kept.bind(1,version);
            while(kept.row()) {
                const auto found=items.find(kept.number(0));
                if(found==items.end()||found->second.slot!=kept.number(1)||found->second.format!=kept.number(2))throw Error("INVALID_ARGUMENT");
            }
        } else if(version>BaseCatalogVersion) {
            // Catalogs grow from the baseline: a later version needs the baseline imported first.
            Statement baseline(store.db(),"SELECT 1 FROM avatar_catalogs WHERE version=?");baseline.bind(1,BaseCatalogVersion);
            if(!baseline.row())throw Error("INVALID_ARGUMENT");
        }
        // Every item of the newest earlier catalog keeps its id and slot.
        Statement previous(store.db(),"SELECT id,slot FROM avatar_catalog_items WHERE version=(SELECT MAX(version) FROM avatar_catalogs WHERE version<?)");
        previous.bind(1,version);
        while(previous.row()) {
            auto found=items.find(previous.number(0));
            if(found==items.end()||found->second.slot!=previous.number(1))throw Error("INVALID_ARGUMENT");
        }
        if(revise) {
            // The replaced revision's files stay in the content-addressed store; a file no catalog
            // lists is no longer served as a catalog file.
            Statement update(store.db(),"UPDATE avatar_catalogs SET manifest=?,revision=?,imported=strftime('%s','now') WHERE version=?");
            update.bind(1,canonical);update.bind(2,revision);update.bind(3,version);(void)update.row();
            for(const auto* sql:{"DELETE FROM avatar_catalog_assets WHERE version=?","DELETE FROM avatar_catalog_items WHERE version=?"}) {
                Statement remove(store.db(),sql);remove.bind(1,version);(void)remove.row();
            }
        } else {
            Statement insert(store.db(),"INSERT INTO avatar_catalogs(version,manifest,revision,imported) VALUES(?,?,?,strftime('%s','now'))");
            insert.bind(1,version);insert.bind(2,canonical);insert.bind(3,revision);(void)insert.row();
        }
        for(const auto& [name,hash]:hashes) {
            const auto& bytes=files.at(name);
            Statement asset(store.db(),"INSERT OR IGNORE INTO assets(hash,mime,size,bytes) VALUES(?,?,?,?)");
            asset.bind(1,hash);asset.bind(2,name.ends_with(".glb")?"model/gltf-binary":"image/png");
            asset.bind(3,static_cast<long long>(bytes.size()));asset.blob(4,bytes);(void)asset.row();
            Statement link(store.db(),"INSERT INTO avatar_catalog_assets(version,name,hash) VALUES(?,?,?)");
            link.bind(1,version);link.bind(2,name);link.bind(3,hash);(void)link.row();
        }
        for(const auto& [id,item]:items) {
            Statement row(store.db(),"INSERT INTO avatar_catalog_items(version,id,slot,format) VALUES(?,?,?,?)");
            row.bind(1,version);row.bind(2,id);row.bind(3,static_cast<long long>(item.slot));row.bind(4,static_cast<long long>(item.format));
            (void)row.row();
        }
        transaction.commit();
    }
    return static_cast<int>(version);
}
}

std::string randomAvatarDescription(sqlite3* db,std::optional<int> bodyType)
{
    Statement latest(db,"SELECT version,manifest FROM avatar_catalogs ORDER BY version DESC LIMIT 1");
    if(!latest.row())throw Error("NOT_FOUND");
    const auto version=latest.number(0);
    const auto manifest=parseAvatarManifest(latest.text(1));
    std::map<long long,int> formats;
    {
        Statement items(db,"SELECT id,format FROM avatar_catalog_items WHERE version=?");items.bind(1,version);
        while(items.row())formats[items.number(0)]=static_cast<int>(items.number(1));
    }
    std::random_device device;std::mt19937 random(device());
    auto pick=[&](const auto& range){return range[std::uniform_int_distribution<std::size_t>(0,range.size()-1)(random)];};
    const int body=bodyType?*bodyType:std::uniform_int_distribution<int>(0,1)(random);
    const char* bodyName=body==1?"male":"female";
    // Wardrobe items and facial hair by slot, with the catalog's per-body CreateRandom weights (1
    // when it has none).
    std::array<std::pair<std::vector<long long>,std::vector<double>>,7> bySlot;
    for(const auto* key:{"items","featureItems"}) {
        if(!manifest.contains(key))continue;
        for(const auto& item:manifest[key]) {
            const auto slot=std::ranges::find(Slots,item.at("slot").get<std::string>())-Slots.begin();
            const double weight=item.contains("random")?item["random"].at(bodyName).get<double>():1.0;
            if(slot>FacialHairSlot||weight<=0)continue;
            bySlot[static_cast<std::size_t>(slot)].first.push_back(item.at("id").get<long long>());
            bySlot[static_cast<std::size_t>(slot)].second.push_back(weight);
        }
    }
    auto choose=[&](std::size_t slot)->long long {
        const auto& [ids,weights]=bySlot[slot];
        if(ids.empty())return 0;
        return ids[std::discrete_distribution<std::size_t>(weights.begin(),weights.end())(random)];
    };
    Fields f;
    f.body=body;
    const int authored=static_cast<int>(manifest.at("bodies").at(bodyName).at("authoredHeightMillimeters").get<long long>());
    f.height=std::clamp(std::uniform_int_distribution<int>(authored-110,authored+110)(random),1450,2050);
    f.build=std::uniform_int_distribution<int>(72,184)(random);
    f.catalog=version;
    f.colors[0]=pick(SkinTones);f.colors[1]=pick(HairColors);f.colors[2]=pick(EyeColors);
    for(std::size_t c=3;c<7;++c)f.colors[c]=pick(ClothColors);
    std::bernoulli_distribution sometimes(0.3);
    for(std::size_t slot=0;slot<6;++slot) {
        f.items[slot]=slot<4||sometimes(random)?choose(slot):0;
        if(slot<4&&f.items[slot]==0)throw Error("INVALID_ARGUMENT");
    }
    // Every face control the catalog has shapes an individual face, and sometimes facial hair.
    if(manifest.contains("faceControls")) {
        std::uniform_real_distribution<double> unit(0.0,1.0);
        for(const auto& control:manifest["faceControls"].at(bodyName)) {
            const auto parameter=static_cast<std::size_t>(control.at("parameter").get<int>());
            f.face[parameter]=std::clamp(Neutral+static_cast<int>(std::lround((unit(random)+unit(random)-1.0)*120.0)),0,255);
        }
    }
    if(std::bernoulli_distribution(0.35)(random))f.facialHair=choose(FacialHairSlot);
    return write(f,lowestFormat(f,[&](long long id){return formats.at(id);}));
}

CatalogInfo catalogInfo(sqlite3* db,long long version)
{
    if(!supportedCatalogVersion(version))throw Error("NOT_FOUND");
    Statement row(db,"SELECT manifest,revision FROM avatar_catalogs WHERE version=?");row.bind(1,version);
    if(!row.row())throw Error("NOT_FOUND");
    CatalogInfo info;
    info.version=version;
    info.revision=row.number(1);
    info.manifest=row.text(0);
    info.manifestSha256=sha256(info.manifest);
    const auto manifest=parseAvatarManifest(info.manifest);
    info.reader=static_cast<int>(manifest.value("reader",1));
    Statement items(db,"SELECT id,slot,format FROM avatar_catalog_items WHERE version=?");items.bind(1,version);
    while(items.row())info.items[items.number(0)]={static_cast<int>(items.number(1)),static_cast<int>(items.number(2))};
    // GCC 14's optimized sanitizer build misdiagnoses vector's one-element initializer-list
    // assignment as an out-of-bounds memmove. The direct append expresses the same intent and
    // avoids that library inlining path.
    info.formats.push_back(1);
    if(manifest.contains("faceControls")||manifest.contains("featureItems"))info.formats.push_back(2);
    if(std::ranges::any_of(info.items,[](const auto& entry){return entry.second.format==3;}))info.formats.push_back(3);
    for(const auto& asset:manifest.at("assets"))info.totalBytes+=asset.at("size").get<long long>();
    return info;
}

Json catalogPack(const CatalogInfo& info)
{
    return Json{{"version",info.version},{"packFormat",1},{"reader",info.reader},{"descriptionFormats",info.formats},
                {"manifestSha256",info.manifestSha256},{"manifestSize",static_cast<long long>(info.manifest.size())},
                {"totalBytes",info.totalBytes}};
}

std::string projectAvatarDescription(std::string_view description,const CatalogInfo& target,int format)
{
    if(description.size()!=AvatarDescriptionSize)return std::string(description);
    if(read16(description,CatalogOffset)==target.version&&static_cast<unsigned char>(description[0])<=format)return std::string(description);
    // What both the client and the target catalog can say.
    const int most=std::clamp(std::min(format,*std::ranges::max_element(target.formats)),1,NewestDescriptionFormat);
    auto f=read(description);
    f.catalog=target.version;
    auto has=[&](long long id,int slot) {
        const auto found=target.items.find(id);
        return found!=target.items.end()&&found->second.slot==slot&&found->second.format<=most;
    };
    auto fallback=[&](int slot)->long long {
        for(const auto& [id,item]:target.items)if(item.slot==slot&&item.format<=most)return id;
        return 0;
    };
    for(int slot=0;slot<6;++slot) {
        auto& id=f.items[static_cast<std::size_t>(slot)];
        if(id!=0&&!has(id,slot))id=slot==GlassesSlot||slot==HatSlot?0:fallback(slot);
    }
    if(most<2||!has(f.facialHair,FacialHairSlot))f.facialHair=0;
    if(most<2)std::fill(f.face.begin(),f.face.begin()+16,Neutral);
    if(most<3) {
        f.eyebrows=f.ears=0;f.accessories={};f.sides={};f.makeup={};f.proportion=Neutral;
        std::fill(f.face.begin()+16,f.face.end(),Neutral);
        std::fill(f.colors.begin()+7,f.colors.end(),Rgb{});
    } else {
        if(!has(f.eyebrows,EyebrowsSlot))f.eyebrows=0;
        if(!has(f.ears,EarsSlot))f.ears=0;
        for(std::size_t i=0;i<4;++i) {
            if(!has(f.accessories[i],EarringsSlot+static_cast<int>(i))){f.accessories[i]=0;f.sides[i]=0;}
            if(!has(f.makeup[i],EyeShadowSlot+static_cast<int>(i)))f.makeup[i]=0;
        }
    }
    return write(f,lowestFormat(f,[&](long long id){return target.items.at(id).format;}));
}

long long storeAvatar(sqlite3* db,const std::string& userId,std::string_view description,long long now)
{
    Statement upsert(db,"INSERT INTO avatars(user_id,description,revision,updated) VALUES(?,?,1,?) "
        "ON CONFLICT(user_id) DO UPDATE SET description=excluded.description,revision=revision+1,updated=excluded.updated "
        "RETURNING revision");
    upsert.bind(1,userId);upsert.blob(2,description);upsert.bind(3,now);
    if(!upsert.row())throw Error("INTERNAL_ERROR");
    return upsert.number(0);
}

void setAvatar(Store& store,const std::string& username,const std::optional<std::string>& description)
{
    Statement user(store.db(),"SELECT id FROM users WHERE username=?");user.bind(1,username);
    if(!user.row())throw Error("NOT_FOUND");
    const auto id=user.text(0);
    if(!description) {
        Statement remove(store.db(),"DELETE FROM avatars WHERE user_id=?");remove.bind(1,id);(void)remove.row();
        return;
    }
    validateAvatarDescription(store.db(),*description);
    Statement now(store.db(),"SELECT strftime('%s','now')");(void)now.row();
    (void)storeAvatar(store.db(),id,*description,now.number(0));
}
}

#include "CnaService/Service.hpp"
#include <set>

namespace CnaService {
namespace {
std::string hexEncode(std::string_view bytes)
{
    constexpr char digits[]="0123456789abcdef";
    std::string text;text.reserve(bytes.size()*2);
    for(unsigned char byte:bytes){text+=digits[byte>>4];text+=digits[byte&15];}
    return text;
}

std::string hexDecode(std::string_view text)
{
    auto nibble=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
    if(text.size()%2)throw Error("INVALID_ARGUMENT");
    std::string bytes;bytes.reserve(text.size()/2);
    for(std::size_t i=0;i<text.size();i+=2) {
        const int high=nibble(text[i]),low=nibble(text[i+1]);
        if(high<0||low<0)throw Error("INVALID_ARGUMENT");
        bytes+=static_cast<char>(high<<4|low);
    }
    return bytes;
}
}

const CatalogInfo& Service::catalog(long long version)
{
    // A catalog version changes only to a higher revision (imported by administration, possibly
    // from another process), so what was derived from one stays true while its revision does. A
    // newer revision is read into the same entry: references taken earlier stay valid.
    if(auto found=catalogs_.find(version);found!=catalogs_.end()) {
        Statement revision(store_.db(),"SELECT revision FROM avatar_catalogs WHERE version=?");revision.bind(1,version);
        if(!revision.row()||revision.number(0)!=found->second.revision)found->second=catalogInfo(store_.db(),version);
        return found->second;
    }
    return catalogs_.emplace(version,catalogInfo(store_.db(),version)).first->second;
}

Json Service::avatars(const std::string& user,const std::string& op,const Json& a,long long now)
{
    if(op=="avatars.get") {
        // Any signed-in account may see any account's avatar, like its gamertag.
        if(!a.contains("userIds")||!a["userIds"].is_array()||a["userIds"].empty()||a["userIds"].size()>16)
            throw Error("INVALID_ARGUMENT");
        // Clients list the description formats they read; the highest this service knows is what it
        // may send. One that lists none reads format 1. An avatar in a higher format is given as
        // that format's copy (projectAvatarDescription): what the client cannot read is left out.
        int format=1;
        if(a.contains("formats")) {
            if(!a["formats"].is_array()||a["formats"].size()>8)throw Error("INVALID_ARGUMENT");
            for(const auto& listed:a["formats"]) {
                if(!listed.is_number_integer())throw Error("INVALID_ARGUMENT");
                const auto value=listed.get<long long>();
                if(value>=1&&value<=NewestDescriptionFormat)format=std::max(format,static_cast<int>(value));
            }
        }
        // A client that says which catalogs it has, and whether and how large it installs the one an
        // avatar names, is given the stored avatar when it can draw it, else a projection onto the
        // newest catalog it has. One that does not say (every client before catalog packs) fetches
        // files one by one and reads the contract level it states, 1 when it states none.
        const bool negotiated=a.contains("catalogs");
        std::set<long long> catalogs;bool updates=false;long long maxBytes=0;
        long long reader=negotiated||a.contains("reader")?integerField(a,"reader",1,1000):1;
        if(negotiated) {
            if(!a["catalogs"].is_array()||a["catalogs"].empty()||a["catalogs"].size()>64)throw Error("INVALID_ARGUMENT");
            for(const auto& version:a["catalogs"]) {
                if(!version.is_number_integer()||version.get<long long>()<1||version.get<long long>()>MaximumCatalogVersion)throw Error("INVALID_ARGUMENT");
                // A development catalog below the baseline names nothing this service has or will
                // ever have; a client draws avatars only from supported catalogs it lists.
                if(supportedCatalogVersion(version.get<long long>()))catalogs.insert(version.get<long long>());
            }
            if(catalogs.empty())throw Error("INVALID_ARGUMENT");
            if(!a.contains("catalogUpdates")||!a["catalogUpdates"].is_boolean())throw Error("INVALID_ARGUMENT");
            updates=a["catalogUpdates"].get<bool>();
            maxBytes=integerField(a,"maxCatalogBytes",0,1LL<<40);
        }
        auto drawable=[&](const CatalogInfo& info) {
            if(!negotiated)return info.reader<=reader;
            return catalogs.contains(info.version)||(updates&&info.reader<=reader&&info.totalBytes<=maxBytes);
        };
        Json avatars=Json::array();
        for(const auto& value:a["userIds"]) {
            if(!value.is_string())throw Error("INVALID_ARGUMENT");
            const auto id=value.get<std::string>();
            if(id.empty()||id.size()>64||id.find_first_not_of("0123456789abcdef")!=std::string::npos)throw Error("INVALID_ARGUMENT");
            Statement avatar(store_.db(),"SELECT description,revision FROM avatars WHERE user_id=?");avatar.bind(1,id);
            if(!avatar.row()) {
                avatars.push_back(Json{{"userId",id},{"description",nullptr},{"revision",0}});
                continue;
            }
            const auto stored=avatar.blob(0);
            const long long version=static_cast<unsigned char>(stored[8])|static_cast<long long>(static_cast<unsigned char>(stored[9]))<<8;
            Json entry{{"userId",id},{"revision",avatar.number(1)}};
            if(drawable(catalog(version))) {
                entry["description"]=hexEncode(projectAvatarDescription(stored,catalog(version),format));
            } else {
                // The newest catalog this client has (or, for an old client, can read).
                const CatalogInfo* target=nullptr;
                Statement versions(store_.db(),"SELECT version FROM avatar_catalogs ORDER BY version DESC");
                while(!target&&versions.row()) {
                    const auto& info=catalog(versions.number(0));
                    if(negotiated?catalogs.contains(info.version):info.reader<=reader)target=&info;
                }
                entry["description"]=hexEncode(target?projectAvatarDescription(stored,*target,format):
                                                       projectAvatarDescription(stored,catalog(version),1));
                entry["projected"]=true;
                entry["catalogVersion"]=version;
            }
            avatars.push_back(std::move(entry));
        }
        return Json{{"avatars",avatars}};
    }
    if(op=="avatars.catalogPack") {
        return catalogPack(catalog(integerField(a,"version",BaseCatalogVersion,MaximumCatalogVersion)));
    }
    if(op=="avatars.set") {
        const auto description=hexDecode(stringField(a,"description",AvatarDescriptionSize*2));
        validateAvatarDescription(store_.db(),description);
        Statement last(store_.db(),"SELECT updated FROM avatars WHERE user_id=?");last.bind(1,user);
        if(last.row()&&now-last.number(0)<2)throw Error("RATE_LIMITED");
        return Json{{"revision",storeAvatar(store_.db(),user,description,now)}};
    }
    // avatars.catalog: the requested (or newest) imported catalog: its version, its manifest's
    // identity, and the manifest itself unless the caller asks only for the version or it does not
    // fit a message (it is then read by hash from the file route, as a pack's is).
    long long version=0;
    if(a.contains("version"))version=integerField(a,"version",0,MaximumCatalogVersion);
    if(version!=0&&!supportedCatalogVersion(version))throw Error("INVALID_ARGUMENT");
    if(a.contains("manifest")&&!a["manifest"].is_boolean())throw Error("INVALID_ARGUMENT");
    Statement catalog(store_.db(),version?"SELECT version,manifest FROM avatar_catalogs WHERE version=?":
        "SELECT version,manifest FROM avatar_catalogs ORDER BY version DESC LIMIT 1");
    if(version)catalog.bind(1,version);
    if(!catalog.row())throw Error("NOT_FOUND");
    const auto manifest=catalog.text(1);
    Json result{{"version",catalog.number(0)},{"manifestSha256",sha256(manifest)},{"manifestSize",static_cast<long long>(manifest.size())}};
    if(a.value("manifest",true)&&manifest.size()+1024<=MaxMessageBytes)result["manifest"]=parseAvatarManifest(manifest);
    return result;
}
}
