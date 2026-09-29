// SPDX-License-Identifier: MIT
// Import-time avatar validation: well-formed models pass, and every way a model or manifest can be
// something a CNA client refuses is refused here first. With CNA_AVATAR_CATALOGS set to a CNA
// checkout's modules/gamer-services/assets/avatars, the real catalogs are imported too.
#include "AvatarFixtures.hpp"
#include "CnaService/AvatarAssets.hpp"
#include "CnaService/Avatars.hpp"
#include "CnaService/Store.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>

using namespace CnaService;
using CnaService::Fixtures::GlbParts;
namespace {
int checks=0;
void check(bool value,const std::string& reason){++checks;if(!value)throw std::runtime_error(reason);}

bool refused(const std::function<void()>& work)
{
    try {work();} catch(const Error& e) {return e.code()=="INVALID_ARGUMENT";}
    return false;
}

// A mutation of a valid body GLB must be refused.
void refuseMutation(const char* reason,const std::function<void(GlbParts&)>& mutate)
{
    auto g=Fixtures::avatarGlb("body",0);
    mutate(g);
    const auto bytes=g.build();
    check(refused([&]{(void)validateAvatarGlb(bytes);}),reason);
}

std::string slurp(const std::filesystem::path& path)
{
    std::ifstream in(path,std::ios::binary);
    std::ostringstream text;text<<in.rdbuf();
    return text.str();
}

int importDirectory(Store& store,const std::filesystem::path& directory)
{
    std::map<std::string,std::string> files;
    for(const auto& entry:std::filesystem::directory_iterator(directory))
        if(entry.path().filename()!="catalog.json")files[entry.path().filename().string()]=slurp(entry.path());
    return importAvatarCatalog(store,slurp(directory/"catalog.json"),files);
}
}

int main()
{
    try {
        // Well-formed models pass.
        const auto body=validateAvatarGlb(Fixtures::avatarGlb("body").build());
        check(body.primitives==1&&body.animations==0,"a minimal body passes");
        check(validateAvatarGlb(Fixtures::avatarGlb("anim",31).build()).animations==31,"animations pass");

        // Container.
        check(refused([]{(void)validateAvatarGlb("glTF");}),"truncated");
        check(refused([]{auto b=Fixtures::avatarGlb("x").build();b[4]=3;(void)validateAvatarGlb(b);}),"glTF version");
        check(refused([]{auto b=Fixtures::avatarGlb("x").build();b.pop_back();(void)validateAvatarGlb(b);}),"length mismatch");
        refuseMutation("external buffer",[](GlbParts& g){g.json["buffers"][0]["uri"]="http://example.invalid/x.bin";});
        refuseMutation("required extension",[](GlbParts& g){g.json["extensionsRequired"]=Json::array({"KHR_draco_mesh_compression"});});
        refuseMutation("external image",[](GlbParts& g){g.json["images"]=Json::array({Json{{"uri","x.png"}}});});
        // Rig.
        refuseMutation("no skin",[](GlbParts& g){g.json.erase("skins");});
        refuseMutation("70 joints",[](GlbParts& g){g.json["skins"][0]["joints"].erase(70);});
        refuseMutation("unknown joint name",[](GlbParts& g){g.json["nodes"][19]["name"]="Skull";});
        refuseMutation("duplicate joint name",[](GlbParts& g){g.json["nodes"][20]["name"]="Head";});
        refuseMutation("wrong topology",[](GlbParts& g){
            // Hang the head off a collar instead of the neck.
            auto& neck=g.json["nodes"][14]["children"];
            neck.erase(std::find(neck.begin(),neck.end(),19));
            g.json["nodes"][12]["children"].push_back(19);
        });
        refuseMutation("rotated bind",[](GlbParts& g){g.json["nodes"][5]["rotation"]={0.0,0.7071,0.0,0.7071};});
        refuseMutation("scaled joint",[](GlbParts& g){g.json["nodes"][5]["scale"]={2.0,1.0,1.0};});
        refuseMutation("absurd translation",[](GlbParts& g){g.json["nodes"][5]["translation"]={0.0,50.0,0.0};});
        // Primitives.
        refuseMutation("lines",[](GlbParts& g){g.json["meshes"][0]["primitives"][0]["mode"]=1;});
        refuseMutation("morph targets",[](GlbParts& g){g.json["meshes"][0]["primitives"][0]["targets"]=Json::array({Json::object()});});
        refuseMutation("extra attribute",[](GlbParts& g){g.json["meshes"][0]["primitives"][0]["attributes"]["COLOR_0"]=0;});
        refuseMutation("missing normals",[](GlbParts& g){g.json["meshes"][0]["primitives"][0]["attributes"].erase("NORMAL");});
        refuseMutation("missing indices",[](GlbParts& g){g.json["meshes"][0]["primitives"][0].erase("indices");});
        refuseMutation("unskinned mesh",[](GlbParts& g){g.json["nodes"][71].erase("skin");});
        refuseMutation("joint index out of range",[](GlbParts& g){
            const auto a=g.accessor(std::vector<std::uint8_t>{90,0,0,0, 19,0,0,0, 19,0,0,0},"VEC4",5121,3);
            g.json["meshes"][0]["primitives"][0]["attributes"]["JOINTS_0"]=a;
            Fixtures::finish(g);
        });
        refuseMutation("zero weights",[](GlbParts& g){
            const auto a=g.accessor(std::vector<std::uint8_t>(12,0),"VEC4",5121,3,true);
            g.json["meshes"][0]["primitives"][0]["attributes"]["WEIGHTS_0"]=a;
            Fixtures::finish(g);
        });
        refuseMutation("index out of range",[](GlbParts& g){
            const auto a=g.accessor(std::vector<std::uint16_t>{0,1,7},"SCALAR",5123,3);
            g.json["meshes"][0]["primitives"][0]["indices"]=a;
            Fixtures::finish(g);
        });
        refuseMutation("float indices",[](GlbParts& g){
            const auto a=g.accessor(std::vector<float>{0,1,2},"SCALAR",5126,3);
            g.json["meshes"][0]["primitives"][0]["indices"]=a;
            Fixtures::finish(g);
        });
        refuseMutation("accessor past its view",[](GlbParts& g){g.json["accessors"][0]["count"]=4;});
        refuseMutation("view past the buffer",[](GlbParts& g){g.json["bufferViews"][0]["byteLength"]=1u<<20;});
        refuseMutation("sparse accessor",[](GlbParts& g){g.json["accessors"][0]["sparse"]=Json::object();});
        refuseMutation("non-finite position",[](GlbParts& g){
            const float bad=std::numeric_limits<float>::infinity();
            const auto a=g.accessor(std::vector<float>{0,0,0, bad,0,0, 0,0.1f,0},"VEC3",5126,3);
            g.json["meshes"][0]["primitives"][0]["attributes"]["POSITION"]=a;
            Fixtures::finish(g);
        });
        refuseMutation("vertex count mismatch",[](GlbParts& g){
            const auto a=g.accessor(std::vector<float>{0,0,1, 0,0,1},"VEC3",5126,2);
            g.json["meshes"][0]["primitives"][0]["attributes"]["NORMAL"]=a;
            Fixtures::finish(g);
        });
        // Materials.
        refuseMutation("unknown tint",[](GlbParts& g){g.json["materials"][0]["extras"]["cnaTint"]="neon";});
        refuseMutation("unknown feature",[](GlbParts& g){g.json["materials"][0]["extras"]["cnaFeature"]="nose";});
        refuseMutation("bad layer",[](GlbParts& g){g.json["materials"][0]["extras"]["cnaLayer"]=4;});
        refuseMutation("texture that is not a PNG",[](GlbParts& g){
            const auto view=g.view("not a png at all, just some bytes....");
            g.json["images"]=Json::array({Json{{"bufferView",view},{"mimeType","image/png"}}});
            g.json["textures"]=Json::array({Json{{"source",0}}});
            g.json["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"]={{"index",0}};
            Fixtures::finish(g);
        });
        refuseMutation("oversized texture",[](GlbParts& g){
            std::string png=Fixtures::atlasPng();
            png.resize((1u<<20)+64,'\0');
            const auto view=g.view(png);
            g.json["images"]=Json::array({Json{{"bufferView",view},{"mimeType","image/png"}}});
            g.json["textures"]=Json::array({Json{{"source",0}}});
            g.json["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"]={{"index",0}};
            Fixtures::finish(g);
        });
        // Animations.
        auto refuseClip=[&](const char* reason,const std::function<void(GlbParts&)>& mutate) {
            auto g=Fixtures::avatarGlb("anim",2);
            mutate(g);
            const auto bytes=g.build();
            check(refused([&]{(void)validateAvatarGlb(bytes);}),reason);
        };
        refuseClip("animation of a mesh node",[](GlbParts& g){g.json["animations"][0]["channels"][0]["target"]["node"]=71;});
        refuseClip("translation off the root",[](GlbParts& g){g.json["animations"][0]["channels"][0]["target"]["path"]="translation";});
        refuseClip("scale channel",[](GlbParts& g){g.json["animations"][0]["channels"][0]["target"]["path"]="scale";});
        refuseClip("linear interpolation",[](GlbParts& g){g.json["animations"][0]["samplers"][0]["interpolation"]="LINEAR";});
        refuseClip("expression out of range",[](GlbParts& g){g.json["animations"][0]["extras"]["cnaExpressions"]=Json::array({Json::array({0,14,0,0,0,0})});});
        refuseClip("expressions out of order",[](GlbParts& g){
            g.json["animations"][0]["extras"]["cnaExpressions"]=Json::array({Json::array({1.0,0,0,0,0,0}),Json::array({0.5,0,0,0,0,0})});
        });
        // Face atlas and controls.
        check(!refused([]{validateFaceAtlas(Fixtures::atlasPng(),Fixtures::atlasLayout());}),"atlas passes");
        check(refused([]{auto l=Fixtures::atlasLayout();l["mouths"].erase("PhoneticL");validateFaceAtlas(Fixtures::atlasPng(),l);}),"missing mouth state");
        check(refused([]{auto l=Fixtures::atlasLayout();l["eyebrows"]["Sad"]["left"]=999;validateFaceAtlas(Fixtures::atlasPng(),l);}),"tile out of range");
        check(refused([]{auto l=Fixtures::atlasLayout();l["width"]=2048;validateFaceAtlas(Fixtures::atlasPng(),l);}),"atlas size mismatch");
        const Json control{{"parameter",3},{"scope","face"},{"ops",Json::array({Json{{"centre",{0,1.5,0.1}},{"radii",{0.05,0.05,0.05}},
                                                                                   {"inner",0.5},{"scale",{1.2,1.2,1.2}}}})}};
        check(!refused([&]{validateFaceControls(Json{{"female",{control}},{"male",{control}}});}),"face controls pass");
        auto badControl=[&](const std::function<void(Json&)>& mutate) {
            auto c=control;mutate(c);
            return refused([&]{validateFaceControls(Json{{"female",{c}},{"male",{control}}});});
        };
        check(badControl([](Json& c){c["ops"][0]["scale"]={9.0,1.0,1.0};}),"scale out of range");
        check(badControl([](Json& c){c["parameter"]=16;}),"parameter out of range");
        check(badControl([](Json& c){c["scope"]="body";}),"unknown scope");
        check(badControl([](Json& c){c["ops"][0]["radii"]={0.0,0.05,0.05};}),"degenerate radius");
        check(badControl([](Json& c){c["ops"][0].erase("scale");c["ops"][0]["rotate"]={0,0,1};c["ops"][0]["degrees"]=180;}),"rotation too large");
        check(refused([&]{validateFaceControls(Json{{"female",{control}}});}),"both body types");

        // Catalog-level: an item not fitted to its body is refused.
        const auto path=std::filesystem::current_path()/"avatar-validation.sqlite3";
        auto clean=[&]{for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(path.string()+suffix);};
        clean();
        {
            Store store(path.string());
            std::map<std::string,std::string> files{{"body.female.glb",Fixtures::avatarGlb("f").build()},
                {"body.male.glb",Fixtures::avatarGlb("m").build()},{"face_features.png",Fixtures::atlasPng()},
                {"animations.glb",Fixtures::avatarGlb("a",31).build()}};
            for(const auto* slot:{"hair","top","bottom","shoes"}) {
                files[std::string(slot)+".female.glb"]=Fixtures::avatarGlb(std::string(slot)+"f").build();
                files[std::string(slot)+".male.glb"]=Fixtures::avatarGlb(std::string(slot)+"m").build();
            }
            auto manifestFor=[&](const std::map<std::string,std::string>& f,int animations=31) {
                (void)animations;
                Json assets=Json::array(), items=Json::array();
                for(const auto& [name,bytes]:f)assets.push_back(Json{{"name",name},{"sha256",sha256(bytes)},{"size",bytes.size()}});
                int id=1;
                for(const auto* slot:{"hair","top","bottom","shoes"})
                    items.push_back(Json{{"id",id++},{"slot",slot},{"name",slot},
                                         {"assets",{{"female",std::string(slot)+".female.glb"},{"male",std::string(slot)+".male.glb"}}}});
                return Json{{"format",1},{"catalogVersion",1},{"rig","cna-avatar-71"},{"assets",assets},{"items",items},
                    {"bodies",{{"female",{{"asset","body.female.glb"},{"authoredHeightMillimeters",1680}}},
                               {"male",{{"asset","body.male.glb"},{"authoredHeightMillimeters",1800}}}}},
                    {"face",{{"asset","face_features.png"},{"layout",Fixtures::atlasLayout()}}},
                    {"animations",{{"asset","animations.glb"},{"presets",Json::array()}}}};
            };
            auto unfitted=files;
            unfitted["top.male.glb"]=Fixtures::avatarGlb("topm",0,1.3f).build();
            check(refused([&]{(void)importAvatarCatalog(store,manifestFor(unfitted).dump(),unfitted);}),"item not fitted to its body");
            auto fewClips=files;
            fewClips["animations.glb"]=Fixtures::avatarGlb("a",30).build();
            check(refused([&]{(void)importAvatarCatalog(store,manifestFor(fewClips).dump(),fewClips);}),"animations need every preset");
            auto brokenBody=files;
            {auto g=Fixtures::avatarGlb("f");g.json["skins"][0]["joints"].erase(3);brokenBody["body.female.glb"]=g.build();}
            check(refused([&]{(void)importAvatarCatalog(store,manifestFor(brokenBody).dump(),brokenBody);}),"malformed body");
            auto badFeature=manifestFor(files);
            badFeature["featureItems"]=Json::array({Json{{"id",50},{"slot","hat"},{"name","x"},
                {"assets",{{"female","hair.female.glb"},{"male","hair.male.glb"}}}}});
            check(refused([&]{(void)importAvatarCatalog(store,badFeature.dump(),files);}),"feature items are facial hair");
            auto badHat=manifestFor(files);
            badHat["items"][1]["hatAssets"]={{"female","hair.female.glb"},{"male","hair.male.glb"}};
            check(refused([&]{(void)importAvatarCatalog(store,badHat.dump(),files);}),"hat variants belong to hair");
            auto badControls=manifestFor(files);
            badControls["faceControls"]=Json{{"female",Json::array({Json{{"parameter",99}}})},{"male",Json::array()}};
            check(refused([&]{(void)importAvatarCatalog(store,badControls.dump(),files);}),"face controls validated at import");
            check(importAvatarCatalog(store,manifestFor(files).dump(),files)==1,"the well-formed catalog imports");
        }
        clean();

        // Cross-repository golden fixtures: the catalogs CNA ships import as they are, in order.
        if(const char* root=std::getenv("CNA_AVATAR_CATALOGS");root&&*root) {
            const auto golden=std::filesystem::current_path()/"avatar-golden.sqlite3";
            for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(golden.string()+suffix);
            Store store(golden.string());
            int imported=0;
            for(int version=1;std::filesystem::is_directory(std::filesystem::path(root)/("v"+std::to_string(version)));++version) {
                check(importDirectory(store,std::filesystem::path(root)/("v"+std::to_string(version)))==version,
                      "CNA catalog v"+std::to_string(version)+" imports");
                ++imported;
            }
            check(imported>=2,"both CNA catalogs found");
            for(int round=0;round<40;++round) {
                const auto random=randomAvatarDescription(store.db(),round%2);
                validateAvatarDescription(store.db(),random);
                validateAvatarDescription(store.db(),formatOneDescription(random));
            }
            std::cout<<"golden: "<<imported<<" CNA catalogs imported\n";
            for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(golden.string()+suffix);
        } else {
            std::cout<<"golden: CNA_AVATAR_CATALOGS not set, CNA catalogs not imported\n";
        }
        std::cout<<"avatar validation "<<checks<<" assertions passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"avatar validation failed: "<<error.what()<<"\n";
        return 1;
    }
}
