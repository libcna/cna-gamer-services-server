// SPDX-License-Identifier: MIT
#include "CnaService/Avatars.hpp"
#include <algorithm>
#include <array>
#include <random>
#include <set>

namespace CnaService {
namespace {
// CNA v1 layout (CNA docs/avatars.md): version, "CNA", body, height mm, build, catalog version,
// 7 RGB colors, 6 item ids, zero reserved bytes, CRC-32 of everything before it.
constexpr std::size_t HeightOffset=5, CatalogOffset=8, ColorOffset=10, ItemOffset=31, ReservedOffset=43,
    ChecksumOffset=AvatarDescriptionSize-4;
constexpr std::array<std::string_view,6> Slots{"hair","top","bottom","shoes","glasses","hat"};
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
    if(d.size()!=AvatarDescriptionSize||d[0]!=1||d.substr(1,3)!="CNA"||static_cast<unsigned char>(d[4])>1)throw Error("INVALID_ARGUMENT");
    const unsigned height=read16(d,HeightOffset);
    if(height<1450||height>2050)throw Error("INVALID_ARGUMENT");
    if(std::any_of(d.begin()+ReservedOffset,d.begin()+ChecksumOffset,[](char c){return c!=0;}))throw Error("INVALID_ARGUMENT");
    std::uint32_t stored=0;
    for(int i=3;i>=0;--i)stored=(stored<<8)|static_cast<unsigned char>(d[ChecksumOffset+i]);
    if(stored!=crc32(d.substr(0,ChecksumOffset)))throw Error("INVALID_ARGUMENT");
    const long long version=read16(d,CatalogOffset);
    Statement catalog(db,"SELECT 1 FROM avatar_catalogs WHERE version=?");catalog.bind(1,version);
    if(!catalog.row())throw Error("INVALID_ARGUMENT");
    for(int slot=0;slot<6;++slot) {
        const long long id=read16(d,ItemOffset+slot*2);
        if(id==0) {if(slot<4)throw Error("INVALID_ARGUMENT");continue;}
        Statement item(db,"SELECT slot FROM avatar_catalog_items WHERE version=? AND id=?");item.bind(1,version);item.bind(2,id);
        if(!item.row()||item.number(0)!=slot)throw Error("INVALID_ARGUMENT");
    }
}

int importAvatarCatalog(Store& store,std::string_view text,const std::map<std::string,std::string>& files)
{
    const auto manifest=parse(text);
    if(!manifest.is_object()||manifest.value("format",0)!=1||manifest.value("rig",std::string())!="cna-avatar-71")
        throw Error("INVALID_ARGUMENT");
    const auto version=integerField(manifest,"catalogVersion",1,65535);
    if(!manifest.contains("assets")||!manifest["assets"].is_array()||manifest["assets"].empty()||manifest["assets"].size()>256)
        throw Error("INVALID_ARGUMENT");
    std::map<std::string,std::string> hashes;
    for(const auto& asset:manifest["assets"]) {
        const auto name=stringField(asset,"name",64), hash=stringField(asset,"sha256",64);
        const auto size=integerField(asset,"size",1,MaximumAssetBytes);
        if(!safeName(name)||hash.size()!=64||hash.find_first_not_of("0123456789abcdef")!=std::string::npos||
           hashes.contains(name))throw Error("INVALID_ARGUMENT");
        auto file=files.find(name);
        if(file==files.end()||static_cast<long long>(file->second.size())!=size||sha256(file->second)!=hash)
            throw Error("INVALID_ARGUMENT");
        checkFile(name,file->second);
        hashes[name]=hash;
    }
    auto listed=[&](const Json& value) {
        if(!value.is_string()||!hashes.contains(value.get<std::string>()))throw Error("INVALID_ARGUMENT");
    };
    for(const auto* body:{"female","male"})listed(manifest.at("bodies").at(body).at("asset"));
    listed(manifest.at("face").at("asset"));
    listed(manifest.at("animations").at("asset"));
    if(!manifest.contains("items")||!manifest["items"].is_array()||manifest["items"].size()>1024)throw Error("INVALID_ARGUMENT");
    std::map<long long,int> items;
    for(const auto& item:manifest["items"]) {
        const auto id=integerField(item,"id",1,65535);
        const auto slot=std::ranges::find(Slots,stringField(item,"slot",16));
        if(slot==Slots.end()||items.contains(id))throw Error("INVALID_ARGUMENT");
        items[id]=static_cast<int>(slot-Slots.begin());
        for(const auto* body:{"female","male"})listed(item.at("assets").at(body));
    }
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

std::string randomAvatarDescription(sqlite3* db,std::optional<int> bodyType)
{
    Statement latest(db,"SELECT MAX(version) FROM avatar_catalogs");
    if(!latest.row()||latest.number(0)==0)throw Error("NOT_FOUND");
    const auto version=latest.number(0);
    std::array<std::vector<long long>,6> bySlot;
    Statement items(db,"SELECT id,slot FROM avatar_catalog_items WHERE version=? ORDER BY id");items.bind(1,version);
    while(items.row())bySlot[static_cast<std::size_t>(items.number(1))].push_back(items.number(0));
    std::random_device device;std::mt19937 random(device());
    auto pick=[&](const auto& range){return range[std::uniform_int_distribution<std::size_t>(0,range.size()-1)(random)];};
    std::string d(AvatarDescriptionSize,'\0');
    d[0]=1;d[1]='C';d[2]='N';d[3]='A';
    const int body=bodyType?*bodyType:std::uniform_int_distribution<int>(0,1)(random);
    d[4]=static_cast<char>(body);
    const int authored=body==1?1800:1680;
    const int height=std::uniform_int_distribution<int>(authored-110,authored+110)(random);
    d[HeightOffset]=static_cast<char>(height&0xff);d[HeightOffset+1]=static_cast<char>(height>>8);
    d[7]=static_cast<char>(std::uniform_int_distribution<int>(72,184)(random));
    d[CatalogOffset]=static_cast<char>(version&0xff);d[CatalogOffset+1]=static_cast<char>(version>>8);
    const std::array<std::array<int,3>,7> colors{pick(SkinTones),pick(HairColors),pick(EyeColors),pick(ClothColors),pick(ClothColors),
        pick(ClothColors),pick(ClothColors)};
    for(std::size_t c=0;c<colors.size();++c)
        for(int k=0;k<3;++k)d[ColorOffset+c*3+k]=static_cast<char>(colors[c][k]);
    std::bernoulli_distribution sometimes(0.3);
    for(std::size_t slot=0;slot<6;++slot) {
        long long id=0;
        if(!bySlot[slot].empty()&&(slot<4||sometimes(random)))id=pick(bySlot[slot]);
        if(slot<4&&id==0)throw Error("INVALID_ARGUMENT");
        d[ItemOffset+slot*2]=static_cast<char>(id&0xff);d[ItemOffset+slot*2+1]=static_cast<char>(id>>8);
    }
    const auto crc=crc32(std::string_view(d).substr(0,ChecksumOffset));
    for(int i=0;i<4;++i)d[ChecksumOffset+i]=static_cast<char>((crc>>(8*i))&0xff);
    return d;
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

Json Service::avatars(const std::string& user,const std::string& op,const Json& a,long long now)
{
    if(op=="avatars.get") {
        // Any signed-in account may see any account's avatar, like its gamertag.
        if(!a.contains("userIds")||!a["userIds"].is_array()||a["userIds"].empty()||a["userIds"].size()>16)
            throw Error("INVALID_ARGUMENT");
        Json avatars=Json::array();
        for(const auto& value:a["userIds"]) {
            if(!value.is_string())throw Error("INVALID_ARGUMENT");
            const auto id=value.get<std::string>();
            if(id.empty()||id.size()>64||id.find_first_not_of("0123456789abcdef")!=std::string::npos)throw Error("INVALID_ARGUMENT");
            Statement avatar(store_.db(),"SELECT description,revision FROM avatars WHERE user_id=?");avatar.bind(1,id);
            if(avatar.row())avatars.push_back(Json{{"userId",id},{"description",hexEncode(avatar.blob(0))},{"revision",avatar.number(1)}});
            else avatars.push_back(Json{{"userId",id},{"description",nullptr},{"revision",0}});
        }
        return Json{{"avatars",avatars}};
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
