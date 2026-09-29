// SPDX-License-Identifier: MIT
#include "CnaService/Avatars.hpp"
#include "CnaService/AvatarAssets.hpp"
#include <algorithm>
#include <cmath>
#include <array>
#include <random>
#include <set>

namespace CnaService {
namespace {
// CNA description layout (CNA docs/avatars.md). Format 1: version, "CNA", body, height mm, build,
// catalog version, 7 RGB colors, 6 item ids, zero reserved bytes, CRC-32 of everything before it.
// Format 2 keeps bytes 1-42 and adds a facial-hair feature item id and 16 face-shape bytes.
constexpr std::size_t HeightOffset=5, CatalogOffset=8, ColorOffset=10, ItemOffset=31, ReservedOffset=43,
    FacialHairOffset=43, FaceOffset=45, FaceCount=16, FaceReservedOffset=FaceOffset+FaceCount,
    ChecksumOffset=AvatarDescriptionSize-4;
// Item slots; slot 6 holds a catalog's feature items (facial hair).
constexpr std::array<std::string_view,7> Slots{"hair","top","bottom","shoes","glasses","hat","facialHair"};
constexpr int FacialHairSlot=6;
constexpr std::size_t MaximumAssetBytes=8u<<20;

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

void setChecksum(std::string& d)
{
    const auto crc=crc32(std::string_view(d).substr(0,ChecksumOffset));
    for(int i=0;i<4;++i)d[ChecksumOffset+i]=static_cast<char>((crc>>(8*i))&0xff);
}

// Palettes the administration generator draws from; a description may carry any RGB value.
constexpr std::array<std::array<int,3>,8> SkinTones{{{255,224,196},{241,194,160},{224,172,128},{198,134,90},{160,104,68},{120,78,52},{92,58,40},{255,210,180}}};
constexpr std::array<std::array<int,3>,8> HairColors{{{36,28,24},{74,48,30},{120,78,40},{176,122,62},{226,188,116},{160,60,36},{200,200,196},{60,64,120}}};
constexpr std::array<std::array<int,3>,6> EyeColors{{{70,46,30},{110,72,40},{60,110,160},{70,130,90},{120,120,130},{40,40,44}}};
constexpr std::array<std::array<int,3>,12> ClothColors{{{220,60,56},{240,150,40},{250,210,60},{90,180,80},{50,150,200},{60,80,180},
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

void validateAvatarDescription(sqlite3* db,std::string_view d)
{
    if(d.size()!=AvatarDescriptionSize||(d[0]!=1&&d[0]!=2)||d.substr(1,3)!="CNA"||static_cast<unsigned char>(d[4])>1)
        throw Error("INVALID_ARGUMENT");
    const bool face=d[0]==2;
    const unsigned height=read16(d,HeightOffset);
    if(height<1450||height>2050)throw Error("INVALID_ARGUMENT");
    if(std::any_of(d.begin()+(face?FaceReservedOffset:ReservedOffset),d.begin()+ChecksumOffset,[](char c){return c!=0;}))
        throw Error("INVALID_ARGUMENT");
    std::uint32_t stored=0;
    for(int i=3;i>=0;--i)stored=(stored<<8)|static_cast<unsigned char>(d[ChecksumOffset+i]);
    if(stored!=crc32(d.substr(0,ChecksumOffset)))throw Error("INVALID_ARGUMENT");
    const long long version=read16(d,CatalogOffset);
    Statement catalog(db,"SELECT 1 FROM avatar_catalogs WHERE version=?");catalog.bind(1,version);
    if(!catalog.row())throw Error("INVALID_ARGUMENT");
    auto slotOf=[&](long long id) {
        Statement item(db,"SELECT slot FROM avatar_catalog_items WHERE version=? AND id=?");item.bind(1,version);item.bind(2,id);
        return item.row()?item.number(0):-1;
    };
    for(int slot=0;slot<6;++slot) {
        const long long id=read16(d,ItemOffset+slot*2);
        if(id==0) {if(slot<4)throw Error("INVALID_ARGUMENT");continue;}
        if(slotOf(id)!=slot)throw Error("INVALID_ARGUMENT");
    }
    if(face) {
        // Format 2 always says something format 1 cannot: facial hair or a shaped face.
        const long long facial=read16(d,FacialHairOffset);
        const bool shaped=std::any_of(d.begin()+FaceOffset,d.begin()+FaceReservedOffset,[](char c){return static_cast<unsigned char>(c)!=128;});
        if(facial==0&&!shaped)throw Error("INVALID_ARGUMENT");
        if(facial!=0&&slotOf(facial)!=FacialHairSlot)throw Error("INVALID_ARGUMENT");
    }
}

std::string formatOneDescription(std::string_view d)
{
    std::string out(d);
    if(out.size()!=AvatarDescriptionSize||out[0]!=2)return out;
    out[0]=1;
    std::fill(out.begin()+FacialHairOffset,out.begin()+FaceReservedOffset,'\0');
    setChecksum(out);
    return out;
}

namespace {
int storeCatalog(Store& store,const Json& manifest,long long version,const std::map<std::string,std::string>& hashes,
    const std::map<std::string,std::string>& files,const std::map<long long,int>& items);
}

int importAvatarCatalog(Store& store,std::string_view text,const std::map<std::string,std::string>& files)
{
    const auto manifest=parse(text);
    if(!manifest.is_object()||manifest.value("format",0)!=1||manifest.value("rig",std::string())!="cna-avatar-71")
        throw Error("INVALID_ARGUMENT");
    const auto version=integerField(manifest,"catalogVersion",1,65535);
    // The contract level these checks enforce; a catalog for newer readers needs a newer service.
    if(manifest.contains("reader")&&integerField(manifest,"reader",1,1)!=1)throw Error("INVALID_ARGUMENT");
    if(!manifest.contains("assets")||!manifest["assets"].is_array()||manifest["assets"].empty()||manifest["assets"].size()>256)
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
        if(model(manifest.at("animations").at("asset")).animations!=31)throw Error("INVALID_ARGUMENT");
        if(manifest.contains("faceControls"))validateFaceControls(manifest["faceControls"]);
        std::map<long long,int> items;
        auto readItems=[&](const char* key,bool feature) {
            if(!manifest.contains(key))return;
            if(!manifest[key].is_array()||manifest[key].size()>1024)throw Error("INVALID_ARGUMENT");
            for(const auto& item:manifest[key]) {
                const auto id=integerField(item,"id",1,65535);
                const auto slot=std::ranges::find(Slots,stringField(item,"slot",16));
                if(slot==Slots.end()||items.contains(id)||(slot-Slots.begin()==FacialHairSlot)!=feature)throw Error("INVALID_ARGUMENT");
                items[id]=static_cast<int>(slot-Slots.begin());
                (void)stringField(item,"name",64);
                fitted(item.at("assets"));
                if(item.contains("random"))
                    for(const auto* body:{"female","male"}) {
                        const double weight=item["random"].at(body).get<double>();
                        if(!(weight>=0&&weight<=1000))throw Error("INVALID_ARGUMENT");
                    }
                if(item.contains("hatAssets")) {
                    if(slot!=Slots.begin())throw Error("INVALID_ARGUMENT");
                    fitted(item["hatAssets"]);
                }
                if(item.contains("coversHair")&&(slot-Slots.begin()!=5||!item["coversHair"].is_boolean()))throw Error("INVALID_ARGUMENT");
            }
        };
        if(!manifest.contains("items"))throw Error("INVALID_ARGUMENT");
        readItems("items",false);
        readItems("featureItems",true);
        for(int required=0;required<4;++required)
            if(std::ranges::none_of(items,[&](const auto& entry){return entry.second==required;}))throw Error("INVALID_ARGUMENT");
        return storeCatalog(store,manifest,version,hashes,files,items);
    } catch(const Json::exception&) {
        throw Error("INVALID_ARGUMENT");
    }
}

namespace {
int storeCatalog(Store& store,const Json& manifest,long long version,const std::map<std::string,std::string>& hashes,
    const std::map<std::string,std::string>& files,const std::map<long long,int>& items)
{
    const auto canonical=manifest.dump();
    store.exec("BEGIN IMMEDIATE");
    try {
        Statement existing(store.db(),"SELECT manifest FROM avatar_catalogs WHERE version=?");existing.bind(1,version);
        if(existing.row()) {
            if(existing.text(0)!=canonical)throw Error("CONFLICT");
            store.exec("COMMIT");
            return static_cast<int>(version);
        }
        // Catalogs only grow: every item of the newest earlier catalog keeps its id and slot.
        Statement previous(store.db(),"SELECT id,slot FROM avatar_catalog_items WHERE version=(SELECT MAX(version) FROM avatar_catalogs WHERE version<?)");
        previous.bind(1,version);
        while(previous.row()) {
            auto found=items.find(previous.number(0));
            if(found==items.end()||found->second!=previous.number(1))throw Error("INVALID_ARGUMENT");
        }
        Statement newer(store.db(),"SELECT 1 FROM avatar_catalogs WHERE version>?");newer.bind(1,version);
        if(newer.row())throw Error("INVALID_ARGUMENT");
        Statement insert(store.db(),"INSERT INTO avatar_catalogs(version,manifest,imported) VALUES(?,?,strftime('%s','now'))");
        insert.bind(1,version);insert.bind(2,canonical);(void)insert.row();
        for(const auto& [name,hash]:hashes) {
            const auto& bytes=files.at(name);
            Statement asset(store.db(),"INSERT OR IGNORE INTO assets(hash,mime,size,bytes) VALUES(?,?,?,?)");
            asset.bind(1,hash);asset.bind(2,name.ends_with(".glb")?"model/gltf-binary":"image/png");
            asset.bind(3,static_cast<long long>(bytes.size()));asset.blob(4,bytes);(void)asset.row();
            Statement link(store.db(),"INSERT INTO avatar_catalog_assets(version,name,hash) VALUES(?,?,?)");
            link.bind(1,version);link.bind(2,name);link.bind(3,hash);(void)link.row();
        }
        for(const auto& [id,slot]:items) {
            Statement row(store.db(),"INSERT INTO avatar_catalog_items(version,id,slot) VALUES(?,?,?)");
            row.bind(1,version);row.bind(2,id);row.bind(3,static_cast<long long>(slot));(void)row.row();
        }
        store.exec("COMMIT");
    } catch(...) {store.exec("ROLLBACK");throw;}
    return static_cast<int>(version);
}
}

std::string randomAvatarDescription(sqlite3* db,std::optional<int> bodyType)
{
    Statement latest(db,"SELECT version,manifest FROM avatar_catalogs ORDER BY version DESC LIMIT 1");
    if(!latest.row())throw Error("NOT_FOUND");
    const auto version=latest.number(0);
    const auto manifest=parse(latest.text(1));
    std::random_device device;std::mt19937 random(device());
    auto pick=[&](const auto& range){return range[std::uniform_int_distribution<std::size_t>(0,range.size()-1)(random)];};
    const int body=bodyType?*bodyType:std::uniform_int_distribution<int>(0,1)(random);
    const char* bodyName=body==1?"male":"female";
    // Items by slot, with the catalog's per-body CreateRandom weights (1 when it has none).
    std::array<std::pair<std::vector<long long>,std::vector<double>>,7> bySlot;
    for(const auto* key:{"items","featureItems"}) {
        if(!manifest.contains(key))continue;
        for(const auto& item:manifest[key]) {
            const auto slot=std::ranges::find(Slots,item.at("slot").get<std::string>())-Slots.begin();
            const double weight=item.contains("random")?item["random"].at(bodyName).get<double>():1.0;
            if(weight<=0)continue;
            bySlot[static_cast<std::size_t>(slot)].first.push_back(item.at("id").get<long long>());
            bySlot[static_cast<std::size_t>(slot)].second.push_back(weight);
        }
    }
    auto choose=[&](std::size_t slot)->long long {
        const auto& [ids,weights]=bySlot[slot];
        if(ids.empty())return 0;
        return ids[std::discrete_distribution<std::size_t>(weights.begin(),weights.end())(random)];
    };
    std::string d(AvatarDescriptionSize,'\0');
    d[1]='C';d[2]='N';d[3]='A';
    d[4]=static_cast<char>(body);
    const int authored=static_cast<int>(manifest.at("bodies").at(bodyName).at("authoredHeightMillimeters").get<long long>());
    const int height=std::clamp(std::uniform_int_distribution<int>(authored-110,authored+110)(random),1450,2050);
    d[HeightOffset]=static_cast<char>(height&0xff);d[HeightOffset+1]=static_cast<char>(height>>8);
    d[7]=static_cast<char>(std::uniform_int_distribution<int>(72,184)(random));
    d[CatalogOffset]=static_cast<char>(version&0xff);d[CatalogOffset+1]=static_cast<char>(version>>8);
    const std::array<std::array<int,3>,7> colors{pick(SkinTones),pick(HairColors),pick(EyeColors),pick(ClothColors),pick(ClothColors),
        pick(ClothColors),pick(ClothColors)};
    for(std::size_t c=0;c<colors.size();++c)
        for(int k=0;k<3;++k)d[ColorOffset+c*3+k]=static_cast<char>(colors[c][k]);
    std::bernoulli_distribution sometimes(0.3);
    for(std::size_t slot=0;slot<6;++slot) {
        const long long id=slot<4||sometimes(random)?choose(slot):0;
        if(slot<4&&id==0)throw Error("INVALID_ARGUMENT");
        d[ItemOffset+slot*2]=static_cast<char>(id&0xff);d[ItemOffset+slot*2+1]=static_cast<char>(id>>8);
    }
    // A catalog with face controls gets an individual face (format 2), and sometimes facial hair.
    bool face=false;
    if(manifest.contains("faceControls")) {
        std::uniform_real_distribution<double> unit(0.0,1.0);
        for(std::size_t k=0;k<FaceCount;++k)
            d[FaceOffset+k]=static_cast<char>(std::clamp(128+static_cast<int>(std::lround((unit(random)+unit(random)-1.0)*120.0)),0,255));
        face=true;
    }
    if(std::bernoulli_distribution(0.35)(random)) {
        const long long id=choose(FacialHairSlot);
        d[FacialHairOffset]=static_cast<char>(id&0xff);d[FacialHairOffset+1]=static_cast<char>(id>>8);
        face=face||id!=0;
    }
    if(face&&!std::any_of(d.begin()+FaceOffset,d.begin()+FaceReservedOffset,[](char c){return static_cast<unsigned char>(c)!=128;})&&
       read16(d,FacialHairOffset)==0)face=false;
    d[0]=face?2:1;
    if(!face)std::fill(d.begin()+FacialHairOffset,d.begin()+FaceReservedOffset,'\0');
    setChecksum(d);
    return d;
}

CatalogInfo catalogInfo(sqlite3* db,long long version)
{
    Statement row(db,"SELECT manifest FROM avatar_catalogs WHERE version=?");row.bind(1,version);
    if(!row.row())throw Error("NOT_FOUND");
    CatalogInfo info;
    info.version=version;
    info.manifest=row.text(0);
    info.manifestSha256=sha256(info.manifest);
    const auto manifest=parse(info.manifest);
    info.reader=static_cast<int>(manifest.value("reader",1));
    info.formats={1};
    if(manifest.contains("faceControls")||manifest.contains("featureItems"))info.formats.push_back(2);
    for(const auto& asset:manifest.at("assets"))info.totalBytes+=asset.at("size").get<long long>();
    Statement items(db,"SELECT id,slot FROM avatar_catalog_items WHERE version=?");items.bind(1,version);
    while(items.row())info.items[items.number(0)]=static_cast<int>(items.number(1));
    return info;
}

Json catalogPack(const CatalogInfo& info)
{
    return Json{{"version",info.version},{"packFormat",1},{"reader",info.reader},{"descriptionFormats",info.formats},
                {"manifestSha256",info.manifestSha256},{"manifestSize",static_cast<long long>(info.manifest.size())},
                {"totalBytes",info.totalBytes}};
}

std::string projectAvatarDescription(std::string_view description,const CatalogInfo& target,bool formatTwo)
{
    std::string out(description);
    if(out.size()!=AvatarDescriptionSize)return out;
    out[CatalogOffset]=static_cast<char>(target.version&0xff);out[CatalogOffset+1]=static_cast<char>(target.version>>8);
    auto slotDefault=[&](int slot)->long long {
        for(const auto& [id,itemSlot]:target.items)if(itemSlot==slot)return id;
        return 0;
    };
    for(int slot=0;slot<6;++slot) {
        long long id=read16(out,ItemOffset+slot*2);
        if(id!=0) {
            const auto found=target.items.find(id);
            if(found==target.items.end()||found->second!=slot)id=slot<4?slotDefault(slot):0;
        }
        out[ItemOffset+slot*2]=static_cast<char>(id&0xff);out[ItemOffset+slot*2+1]=static_cast<char>(id>>8);
    }
    const bool faceTarget=formatTwo&&std::ranges::find(target.formats,2)!=target.formats.end();
    if(out[0]==2&&faceTarget) {
        const long long facial=read16(out,FacialHairOffset);
        const auto found=target.items.find(facial);
        if(facial!=0&&(found==target.items.end()||found->second!=FacialHairSlot)) {
            out[FacialHairOffset]=0;out[FacialHairOffset+1]=0;
        }
        const bool shaped=std::any_of(out.begin()+FaceOffset,out.begin()+FaceReservedOffset,[](char c){return static_cast<unsigned char>(c)!=128;});
        if(!shaped&&read16(out,FacialHairOffset)==0)out[0]=1;
    } else {
        out[0]=1;
    }
    if(out[0]==1)std::fill(out.begin()+FacialHairOffset,out.begin()+FaceReservedOffset,'\0');
    setChecksum(out);
    return out;
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
    // Imported catalogs never change, so what was derived from one stays true.
    if(auto found=catalogs_.find(version);found!=catalogs_.end())return found->second;
    return catalogs_.emplace(version,catalogInfo(store_.db(),version)).first->second;
}

Json Service::avatars(const std::string& user,const std::string& op,const Json& a,long long now)
{
    if(op=="avatars.get") {
        // Any signed-in account may see any account's avatar, like its gamertag.
        if(!a.contains("userIds")||!a["userIds"].is_array()||a["userIds"].empty()||a["userIds"].size()>16)
            throw Error("INVALID_ARGUMENT");
        // Clients list the description formats they read; one that does not (or cannot read format
        // 2) is given the format 1 copy: the same body, colours and items without facial hair or
        // face shape.
        bool formatTwo=false;
        if(a.contains("formats")) {
            if(!a["formats"].is_array()||a["formats"].size()>8)throw Error("INVALID_ARGUMENT");
            for(const auto& format:a["formats"]) {
                if(!format.is_number_integer())throw Error("INVALID_ARGUMENT");
                formatTwo=formatTwo||format.get<int>()==2;
            }
        }
        // A client that says which catalogs it has, and whether and how large it installs the one an
        // avatar names, is given the stored avatar when it can draw it, else a projection onto the
        // newest catalog it has. One that does not say (every client before catalog packs) fetches
        // files one by one and reads contract level 1 only.
        const bool negotiated=a.contains("catalogs");
        std::set<long long> catalogs;bool updates=false;long long maxBytes=0,reader=1;
        if(negotiated) {
            if(!a["catalogs"].is_array()||a["catalogs"].empty()||a["catalogs"].size()>64)throw Error("INVALID_ARGUMENT");
            for(const auto& version:a["catalogs"]) {
                if(!version.is_number_integer()||version.get<long long>()<1||version.get<long long>()>65535)throw Error("INVALID_ARGUMENT");
                catalogs.insert(version.get<long long>());
            }
            if(!a.contains("catalogUpdates")||!a["catalogUpdates"].is_boolean())throw Error("INVALID_ARGUMENT");
            updates=a["catalogUpdates"].get<bool>();
            maxBytes=integerField(a,"maxCatalogBytes",0,1LL<<40);
            reader=integerField(a,"reader",1,1000);
        }
        auto drawable=[&](const CatalogInfo& info) {
            if(!negotiated)return info.reader<=1;
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
                entry["description"]=hexEncode(formatTwo?stored:formatOneDescription(stored));
            } else {
                // The newest catalog this client has (or, for an old client, can read).
                const CatalogInfo* target=nullptr;
                Statement versions(store_.db(),"SELECT version FROM avatar_catalogs ORDER BY version DESC");
                while(!target&&versions.row()) {
                    const auto& info=catalog(versions.number(0));
                    if(negotiated?catalogs.contains(info.version):info.reader<=1)target=&info;
                }
                entry["description"]=hexEncode(target?projectAvatarDescription(stored,*target,formatTwo):formatOneDescription(stored));
                entry["projected"]=true;
                entry["catalogVersion"]=version;
            }
            avatars.push_back(std::move(entry));
        }
        return Json{{"avatars",avatars}};
    }
    if(op=="avatars.catalogPack") {
        return catalogPack(catalog(integerField(a,"version",1,65535)));
    }
    if(op=="avatars.set") {
        const auto description=hexDecode(stringField(a,"description",AvatarDescriptionSize*2));
        validateAvatarDescription(store_.db(),description);
        Statement last(store_.db(),"SELECT updated FROM avatars WHERE user_id=?");last.bind(1,user);
        if(last.row()&&now-last.number(0)<2)throw Error("RATE_LIMITED");
        return Json{{"revision",storeAvatar(store_.db(),user,description,now)}};
    }
    // avatars.catalog: the requested (or newest) imported catalog manifest.
    long long version=0;
    if(a.contains("version"))version=integerField(a,"version",0,65535);
    Statement catalog(store_.db(),version?"SELECT version,manifest FROM avatar_catalogs WHERE version=?":
        "SELECT version,manifest FROM avatar_catalogs ORDER BY version DESC LIMIT 1");
    if(version)catalog.bind(1,version);
    if(!catalog.row())throw Error("NOT_FOUND");
    return Json{{"version",catalog.number(0)},{"manifest",parse(catalog.text(1))}};
}
}
