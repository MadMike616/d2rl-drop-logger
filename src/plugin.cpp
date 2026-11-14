#include <D2RLPlugin/api.h>
#include "drop_logger_breadcrumbs.h"
#include "native_witnesses.hpp"
#include "chest_loot_witnesses.hpp"
#include "treasure_drop_witnesses.hpp"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace {
constexpr D2RL::PluginInfo Info{
    .infoSize = D2RL::PluginInfoSize, .abiVersion = D2RL_PLUGIN_ABI_VERSION,
    .id = DropLogger::PluginId, .name = "Drop Logger",
    .version = "1.0.1", .author = "MadMike",
    .description = "Read-only observation of container openings, monster deaths, and ground items.",
    .flags = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};
const D2RL::PluginContext* Context{};
const D2RL::ThreadService* Threads{};
const D2RL::LifecycleService* Lifecycle{};
const D2RL::LocalizationService* Localization{};
const D2RL::DiagnosticsService* Diagnostics{};
using OperateFn = std::int32_t(__fastcall*)(void*, void*, std::uint32_t, std::uint32_t, bool);
using TreasureDropFn = void*(__fastcall*)(void*, void*, std::int32_t, std::int32_t, std::int32_t, std::int32_t);
using UnitFn = void*(__fastcall*)(void*);
using LookupFn = void*(__fastcall*)(void*, std::uint32_t, std::uint32_t);
using NeighboursFn = void(__fastcall*)(void*, void***, std::uint32_t*);
using CodeFn = std::uint32_t(__fastcall*)(void*);
using CoordFn = std::int32_t(__fastcall*)(void*);
using StatFn = std::int32_t(__cdecl*)(void*, std::int32_t, std::int32_t) noexcept;
using GetLocalDataContextFn = std::int32_t(__fastcall*)();
using GetLocalPlayerFn = void*(__fastcall*)(std::int32_t);
OperateFn Original{};
TreasureDropFn OriginalTreasureDrop{};
UnitFn Room{}, First{}, Next{}, GameFromUnit{}, ObjectRecord{};
LookupFn Lookup{};
NeighboursFn Neighbours{};
CodeFn ItemCode{};
CoordFn CoordX{}, CoordY{};
StatFn ReadStat{};
GetLocalDataContextFn GetLocalDataContext{};
GetLocalPlayerFn GetLocalPlayer{};
D2RL::Reimplementations::GetLevelIdFromUnitFn LevelId{};
std::array<D2RL::Lifecycle::ListenerHandle, 3> Listeners{};
D2RL::Lifecycle::ListenerHandle DeathListener{};
std::atomic<bool> Active{}, InGame{}, Queued{};
std::atomic<std::uint64_t> Epoch{}, Sequence{}, DeathSequence{}, LostRecords{};
std::atomic<std::uint32_t> LocalPlayerId{D2RL::Lifecycle::InvalidUnitId};
HANDLE StopEvent{}, Worker{};
std::mutex QueueMutex;
struct LogRecord { std::string json, readable; };
std::deque<LogRecord> Queue;
std::filesystem::path OutputDirectory;
std::string Capture;
// Game heading state is used by the UI-thread gameplay callbacks.
std::uint64_t GameNumber{};
std::optional<std::uint8_t> GameDifficulty;
bool GameHeadingPending{};
bool GameDifficultyLayout{};
bool ItemQualityLayout{};
bool MonsterPositions{};
bool ChestFlagsLayout{}, ChestLockLayout{};
thread_local bool InsideObserver{};
constexpr std::size_t MaxItems = 1024;
constexpr std::uint64_t WindowMs = 2000;
constexpr std::uint64_t BaselineIntervalMs = 100;
constexpr std::size_t MaxPendingDeaths = 256;
constexpr std::size_t SessionsToKeep = 5;
constexpr std::array<int, 47> Stats{
    0,1,2,3,6,7,8,9,14,16,17,19,21,22,23,24,25,26,27,28,29,31,
    39,40,41,42,43,44,45,46,48,49,50,51,52,53,54,55,57,58,
    60,62,70,72,73,80,194};

template<class T> bool Read(const void* p, std::size_t offset, T& value) noexcept {
    if (!p) return false;
    const auto at = reinterpret_cast<std::uintptr_t>(p);
    if (offset > UINTPTR_MAX-at || sizeof(T) > UINTPTR_MAX-at-offset) return false;
    SIZE_T copied{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(at+offset),
        &value, sizeof(value), &copied) != FALSE && copied == sizeof(value);
}
std::string Quote(std::string_view s) {
    std::string out{"\""};
    for (unsigned char c : s) {
        if (c == '\\' || c == '"') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { char b[7]; std::snprintf(b,sizeof(b),"\\u%04x",c); out += b; }
        else out += static_cast<char>(c);
    }
    return out+'"';
}
std::string Prefix(const char* type, std::uint64_t run) {
    return "{\"type\":"+Quote(type)+",\"capture\":"+Quote(Capture)
        +",\"run\":"+std::to_string(run)+",\"tick_ms\":"+std::to_string(GetTickCount64());
}
bool Emit(std::string record, std::string readable) {
    std::lock_guard lock(QueueMutex);
    if (Queue.size() >= 8192) { LostRecords.fetch_add(1); return false; }
    Queue.push_back({std::move(record),std::move(readable)});
    return true;
}

template<std::size_t Size>
bool BoundedText(const char (&field)[Size],std::string_view& value) noexcept {
    const auto* end=std::find(field,field+Size,'\0');
    if(end==field+Size) return false;
    value={field,static_cast<std::size_t>(end-field)};
    return value.empty() || MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,field,
        static_cast<int>(value.size()),nullptr,0)>0;
}

std::string SingleLine(std::string_view value) {
    std::string result(value);
    for(auto& ch:result) if(static_cast<unsigned char>(ch)<32 || ch==127) ch=' ';
    return result;
}

std::string PlainItemName(std::string_view text) {
    std::string name;
    name.reserve(text.size());
    while(!text.empty()) {
        std::size_t marker{};
        // The loader converts localization's ÿc to U+E07E. Each form takes one ASCII selector.
        if(text.starts_with("\xEE\x81\xBE") || text.starts_with("\xC3\xBF" "c")) marker=3;
        else if(text.starts_with("\xFF" "c")) marker=2;
        if(marker && text.size()>marker && text[marker]>='!' && text[marker]<='~') {
            text.remove_prefix(marker+1);
        } else {
            name+=text.front();
            text.remove_prefix(1);
        }
    }
    return name;
}

DropLogger::BreadcrumbResult __cdecl WriteBreadcrumb(
    const D2RL::PluginContext* caller,const DropLogger::Breadcrumb* request) noexcept {
    using enum DropLogger::BreadcrumbResult;
    using enum DropLogger::BreadcrumbKind;
    if(!Active.load()) return Unavailable;
    if(!D2RL::HasContext(caller) || !caller->pluginId || !caller->pluginId[0]
        || !request || request->structSize<sizeof(*request)) return InvalidArgument;
    std::string_view version,action,message;
    if(!BoundedText(request->pluginVersion,version) || version.empty()
        || !BoundedText(request->action,action) || !BoundedText(request->message,message))
        return InvalidArgument;
    try {
        auto identity=",\"plugin_id\":"+Quote(caller->pluginId)+",\"plugin_version\":"+Quote(version);
        auto label=SingleLine(caller->pluginId)+" "+SingleLine(version);
        if(request->kind==IdentifyTestPlugin)
            return Emit(Prefix("test_plugin",0)+identity+"}","Test plugin: "+label)?Accepted:QueueFull;
        if(request->kind!=Marker || action.empty()) return InvalidArgument;
        const auto run=InGame.load()?Epoch.load():0;
        const auto correlation=std::to_string(request->correlationId);
        auto line="  ["+label+"] "+SingleLine(action);
        if(request->correlationId) line+=" #"+correlation;
        if(!message.empty()) line+=" | "+SingleLine(message);
        return Emit(Prefix("plugin_breadcrumb",run)+identity+",\"action\":"+Quote(action)
            +",\"correlation_id\":"+correlation+",\"message\":"+Quote(message)+"}",std::move(line))
            ?Accepted:QueueFull;
    } catch(...) {
        LostRecords.fetch_add(1);
        return Unavailable;
    }
}

bool PublishBreadcrumbs() noexcept {
    const D2RL::PluginCommunicationService* communication{};
    if(Context->QueryService(&communication)!=D2RL::ServiceQueryResult::Success
        || !D2RL::HasPluginCommunicationServiceField(communication,D2RL::PluginCommunicationServiceRequiredSize)
        || !communication->publishService) return false;
    static constexpr DropLogger::BreadcrumbsV1 table{WriteBreadcrumb};
    const D2RL::PluginCommunication::PublishServiceRequest request{
        .structSize=D2RL::PluginCommunication::PublishServiceRequestSize,
        .name=DropLogger::BreadcrumbService,.serviceVersion=DropLogger::BreadcrumbVersion,
        .tableSize=sizeof(table),.table=&table};
    return communication->publishService(Context,&request)==D2RL::PluginCommunication::Result::Success;
}
struct RawItem {
    std::uint32_t id{}, klass{}, code{}, mode{};
    std::int32_t x{}, y{}, areaId{-1};
    std::optional<std::uint32_t> quality;
    std::array<std::int32_t, Stats.size()> stats{};
};

void ReadItemDetails(void* unit,RawItem& item) {
    item.code=ItemCode(unit); item.x=CoordX(unit); item.y=CoordY(unit);
    item.areaId=LevelId(unit);
    const void* data{};
    std::uint32_t quality{};
    item.quality=ItemQualityLayout && Read(unit,NativeWitnesses::ItemDataOffset,data) && Read(data,0,quality)
        ?std::optional{quality}:std::nullopt;
    for(std::size_t s=0;s<Stats.size();++s) item.stats[s]=ReadStat(unit,Stats[s],0);
}

struct ExactDrop {
    std::uint32_t monster{}, monsterClass{};
    std::int32_t level{-1};
    RawItem item{};
};
std::vector<ExactDrop> PendingExactDrops;
struct Snapshot {
    std::array<RawItem,MaxItems> items{};
    std::size_t count{};
    bool complete{true};
};
enum class ScanStatus { Skipped, Failed, Incomplete, Complete };

ScanStatus ResultOfScan(bool captured,const Snapshot& snapshot) noexcept {
    if(!captured) return ScanStatus::Failed;
    return snapshot.complete?ScanStatus::Complete:ScanStatus::Incomplete;
}

const char* ScanLabel(ScanStatus status) noexcept {
    switch(status) {
    case ScanStatus::Complete: return "complete";
    case ScanStatus::Incomplete: return "incomplete";
    case ScanStatus::Failed: return "failed";
    default: return "skipped";
    }
}
// All helper calls and snapshots below run on the authoritative game thread.
// This SEH boundary contains POD locals only and never encloses the original game call.
bool TakeSnapshot(void* room, Snapshot* out, bool details,
        const std::unordered_set<std::uint32_t>* known=nullptr) noexcept {
    __try {
        if (!room || !out) return false;
        void** neighbours{}; std::uint32_t roomCount{};
        Neighbours(room,&neighbours,&roomCount);
        if (roomCount > 128 || (roomCount && !neighbours)) return false;
        std::size_t totalVisited{};
        for (std::uint32_t r=0; r<=roomCount; ++r) {
            void* current = room;
            if (r && !Read(neighbours,(r-1)*sizeof(void*),current)) return false;
            if (!current) continue;
            if (r && current==room) continue;
            void* unit=First(current);
            std::size_t visited{};
            while (unit) {
                if (++visited > 8192 || ++totalVisited > 16384) { out->complete=false; return true; }
                std::uint32_t type{}, mode{}, id{}, klass{};
                if (!Read(unit,0,type) || !Read(unit,4,klass) || !Read(unit,8,id)
                    || !Read(unit,12,mode)) return false;
                if (type==4 && (mode==3 || mode==5)) {
                    bool duplicate{};
                    for (std::size_t i=0;i<out->count;++i) if (out->items[i].id==id) {duplicate=true; break;}
                    if (!duplicate) {
                        if (out->count==MaxItems) {out->complete=false; break;}
                        auto& item=out->items[out->count++];
                        item.id=id; item.klass=klass; item.mode=mode;
                        if(details && (!known || !known->contains(id))) ReadItemDetails(unit,item);
                    }
                }
                void* next=Next(unit);
                if (next==unit) return false;
                unit=next;
            }
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct Coverage {
    std::uint32_t complete{}, incomplete{}, failed{}, skipped{}, candidates{};
    void Add(ScanStatus status) noexcept {
        switch(status) {
        case ScanStatus::Complete: ++complete; break;
        case ScanStatus::Incomplete: ++incomplete; break;
        case ScanStatus::Failed: ++failed; break;
        case ScanStatus::Skipped: ++skipped; break;
        }
    }
};

struct Opening {
    std::uint64_t order{}, tick{}, run{};
    std::uint32_t object{}, klass{}, before{}, operation{}, init{};
    int x{},y{},level{};
    const char* kind{"object"};
    std::array<char,64> className{};
    bool chestFlagsKnown{}, superChest{};
    enum class Lock { Unknown, Unlocked, Locked } lock{};
    void* room{};
    void* game{};
    std::uint32_t player{};
    Coverage coverage{};
};
struct Death {
    std::uint64_t order{}, tick{};
    D2RL::Lifecycle::MonsterDeathEvent event{};
    int x{}, y{};
    Coverage coverage{};
    const char* positionStatus{"capture_unavailable"};
};
std::vector<Opening> Pending;
std::deque<Death> PendingDeaths;
std::unordered_set<std::uint32_t> SeenItems;
std::unordered_set<std::uint32_t> OpenedObjects;
std::uint64_t StateEpoch{};
std::uint64_t LastBaseline{};

void* ActiveGame() noexcept {
    void* game{};
    return Read(reinterpret_cast<const void*>(Context->exeBase),NativeWitnesses::ActiveGameRva,game)
        ?game:nullptr;
}

std::optional<std::uint8_t> ReadGameDifficulty() noexcept {
    if(!GameDifficultyLayout) return std::nullopt;
    const auto* game=ActiveGame();
    std::uint8_t difficulty{};
    if(Read(game,NativeWitnesses::GameDifficultyOffset,difficulty) && difficulty<=2 && ActiveGame()==game)
        return difficulty;
    return std::nullopt;
}

bool ReadCharacterName(std::uint32_t playerId,char (&name)[NativeWitnesses::CharacterNameCapacity]) noexcept {
    if(!GetLocalPlayer || playerId==D2RL::Lifecycle::InvalidUnitId) return false;
    __try {
        const auto index=GetLocalDataContext();
        if(index<0 || index>=8) return false;
        const auto* player=GetLocalPlayer(index);
        std::uint32_t type{},id{};
        const void* data{};
        return Read(player,0,type) && type==0 && Read(player,8,id) && id==playerId
            && Read(player,NativeWitnesses::PlayerDataOffset,data) && Read(data,0,name);
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}

// Called only on the authoritative thread inside a guarded native read.
void* FindObservedUnit(void* game,D2RL::Lifecycle::UnitType type,std::uint32_t id) {
    if(!game || id==D2RL::Lifecycle::InvalidUnitId) return nullptr;
    const auto unitType=static_cast<std::uint32_t>(type);
    auto* unit=Lookup(game,unitType,id);
    std::uint32_t actualType{},actualId{};
    if(!Read(unit,0,actualType) || actualType!=unitType || !Read(unit,8,actualId)
        || actualId!=id || GameFromUnit(unit)!=game) return nullptr;
    return unit;
}

void* DeathRoom(Death& death,bool locate) noexcept {
    __try {
        const auto& event=death.event;
        death.positionStatus="event_area_unavailable";
        if(event.levelId<0) return nullptr;
        death.positionStatus="active_game_unavailable";
        auto* game=ActiveGame();
        if(!game) return nullptr;
        death.positionStatus="unit_lookup_failed";
        auto* unit=FindObservedUnit(game,event.monster.type,event.monster.id);
        if(!unit) return nullptr;
        death.positionStatus="monster_class_mismatch";
        std::uint32_t klass{};
        if(!Read(unit,4,klass) || klass!=event.monster.classId) return nullptr;
        death.positionStatus="monster_area_mismatch";
        if(LevelId(unit)!=event.levelId) return nullptr;
        death.positionStatus="monster_room_unavailable";
        auto* room=Room(unit);
        if(!room) return nullptr;
        if(locate) {death.x=CoordX(unit); death.y=CoordY(unit);}
        death.positionStatus="resolved";
        return room;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        death.positionStatus="native_read_failed";
        return nullptr;
    }
}

void* PlayerRoom() noexcept {
    __try {
        auto* player=FindObservedUnit(ActiveGame(),D2RL::Lifecycle::UnitType::Player,LocalPlayerId.load());
        return player?Room(player):nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return nullptr;}
}

void ReadObjectClass(const std::uint8_t* record,Opening& opening) noexcept {
    // Objects.bin rows carry the linked class ID followed by its 64-byte key.
    std::uint16_t classId{};
    std::array<char,64> name{};
    if(!Read(record,0,classId) || classId!=opening.klass || !Read(record,2,name)) return;
    const auto end=std::find(name.begin(),name.end(),'\0');
    if(end==name.begin() || end==name.end()) return;
    for(auto ch=name.begin();ch!=end;++ch) {
        if(!((*ch>='A' && *ch<='Z') || (*ch>='a' && *ch<='z') || (*ch>='0' && *ch<='9')
            || *ch=='_' || *ch=='-' || *ch=='.')) return;
    }
    opening.className=name;
}

void ReadChestRule(const void* object,Opening& opening) noexcept {
    std::uint8_t flags{};
    if(ChestFlagsLayout && Read(object,0xC8,flags)) {
        opening.chestFlagsKnown=true;
        opening.superChest=(flags&1)!=0;
    }
    void* data{};
    if(ChestLockLayout && Read(object,0x10,data) && Read(data,8,flags))
        opening.lock=(flags&0x80)!=0?Opening::Lock::Locked:Opening::Lock::Unlocked;
}

struct LootRule { const char* key; const char* label; };

LootRule RuleFor(const Opening& opening) noexcept {
    switch(opening.operation) {
    case 3: return {"breakable_container","breakable container"};
    case 4:
        if(opening.klass==397) return {"sparkly_chest","sparkly chest"};
        if(!opening.chestFlagsKnown) return {"unknown_chest","chest rule unknown"};
        if(opening.superChest) return {"super_chest","super chest"};
        return {"regular_chest","regular chest"};
    case 5: return {"barrel","barrel"};
    case 14: return {"scenery_container","scenery container"};
    case 19: return {"armor_rack","armor rack"};
    case 20: return {"weapon_rack","weapon rack"};
    case 51: return {"jungle_pillar","jungle pillar"};
    default: return {"unknown","loot rule unknown"};
    }
}

std::string OpeningLabel(const Opening& opening) {
    auto label=opening.className[0]?std::string(opening.className.data()):"class "+std::to_string(opening.klass);
    label+=" [";
    label+=RuleFor(opening).label;
    if(opening.operation==4) {
        if(opening.lock==Opening::Lock::Locked) label+=", locked";
        else if(opening.lock==Opening::Lock::Unlocked) label+=", unlocked";
        else label+=", lock state unknown";
    }
    return label+']';
}

bool GetOpening(void* game,void* player,std::uint32_t objectId,Opening* out) noexcept {
    __try {
        std::uint32_t type{},id{},klass{},mode{};
        if (!Read(player,0,type) || type!=0 || GameFromUnit(player)!=game || !Read(player,8,id)) return false;
        void* object=Lookup(game,2,objectId);
        if (!object || !Read(object,0,type) || type!=2 || !Read(object,4,klass)
            || !Read(object,12,mode)) return false;
        auto* record=static_cast<const std::uint8_t*>(ObjectRecord(object));
        if (!record) return false;
        // Objects row access and operation-byte offsets are covered by the copied native witnesses.
        const auto op=record[0x15E];
        const char* kind=nullptr;
        switch(op) {
        case 4: kind="chest"; break;
        case 19: kind="armor_rack"; break;
        case 20: kind="weapon_rack"; break;
        case 3: case 5: case 14: case 51: kind="scenery_container"; break;
        default: return false;
        }
        out->game=game; out->player=id; out->object=objectId; out->klass=klass;
        out->before=mode; out->operation=op; out->init=record[0x15C];
        ReadObjectClass(record,*out);
        if(op==4) ReadChestRule(object,*out);
        out->kind=kind; out->x=CoordX(object); out->y=CoordY(object);
        out->level=LevelId(object); out->room=Room(object);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool AfterMode(const Opening& open,std::uint32_t& mode) noexcept {
    __try {void* object=Lookup(open.game,2,open.object); return Read(object,12,mode);}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
std::string OpeningFields(const Opening& open) {
    auto fields=",\"opening\":"+std::to_string(open.order)+",\"object_id\":"+std::to_string(open.object)
        +",\"class_id\":"+std::to_string(open.klass)+",\"kind\":"+Quote(open.kind)
        +",\"class_name\":"+(open.className[0]?Quote(open.className.data()):"null")
        +",\"loot_rule\":"+Quote(RuleFor(open).key)
        +",\"operate_fn\":"+std::to_string(open.operation)+",\"init_fn\":"+std::to_string(open.init)
        +",\"level\":"+std::to_string(open.level)+",\"x\":"+std::to_string(open.x)+",\"y\":"+std::to_string(open.y);
    if(open.operation==4) {
        fields+=",\"chest_locked\":";
        fields+=open.lock==Opening::Lock::Unknown?"null":(open.lock==Opening::Lock::Locked?"true":"false");
    }
    return fields;
}
LogRecord DescribeItem(const RawItem& item) {
    std::string code;
    for (unsigned i=0;i<4;++i) {const auto c=static_cast<unsigned char>(item.code>>(i*8)); if(c>=33 && c<127) code+=static_cast<char>(c);}
    char name[256]{};
    const auto key="d2r:"+code;
    if (Localization && Localization->getStringByKey)
        (void)Localization->getStringByKey(Context,key.c_str(),name,sizeof(name),nullptr);
    name[sizeof(name)-1]=0;
    auto baseName=PlainItemName(name);
    if(baseName.empty()) baseName=code;
    constexpr std::array qualities{"", "inferior", "normal", "superior", "magic", "set", "rare", "unique", "crafted"};
    static_assert(qualities.size()==static_cast<std::size_t>(D2RL::Items::Quality::Crafted)+1);
    const std::string_view quality=item.quality && *item.quality<qualities.size()?qualities[*item.quality]:"";
    std::string out=",\"item_id\":"+std::to_string(item.id)+",\"item_class\":"+std::to_string(item.klass)
        +",\"code\":"+Quote(code)+",\"base_name\":"+Quote(baseName)
        +",\"quality\":"+(quality.empty()?"null":Quote(quality))
        +",\"quality_id\":"+(item.quality?std::to_string(*item.quality):"null")
        +",\"item_x\":"+std::to_string(item.x)+",\"item_y\":"+std::to_string(item.y)
        +",\"item_area_id\":"+std::to_string(item.areaId)
        +",\"item_mode\":"+std::to_string(item.mode)+",\"stats_layer0\":{";
    std::string readable=baseName.empty()?"Item class "+std::to_string(item.klass):baseName;
    if(!code.empty() && baseName!=code) readable+=" ["+code+"]";
    readable=SingleLine(readable);
    readable+=" (";
    readable+=quality.empty()?"quality unknown":quality;
    readable+=')';
    bool first=true;
    for(std::size_t i=0;i<Stats.size();++i) if(item.stats[i]!=0) {
        if(!first) out+=','; first=false;
        out+=Quote(std::to_string(Stats[i]))+':'+std::to_string(item.stats[i]);
        if(Stats[i]==14) readable+=" ("+std::to_string(item.stats[i])+" gold)";
        if(Stats[i]==70) readable+=" (qty "+std::to_string(item.stats[i])+")";
        if(Stats[i]==194) readable+=" ("+std::to_string(item.stats[i])+" sockets)";
    }
    readable+=" at "+std::to_string(item.x)+", "+std::to_string(item.y)
        +" (area "+std::to_string(item.areaId)+")";
    return {out+'}',std::move(readable)};
}
void SyncEpoch() {
    const auto now=Epoch.load();
    if(now==StateEpoch) return;
    Pending.clear(); PendingDeaths.clear(); PendingExactDrops.clear(); SeenItems.clear(); OpenedObjects.clear();
    LastBaseline=0; StateEpoch=now;
}

bool ReadTreasureSource(void* game,void* source,ExactDrop& drop) noexcept {
    __try {
        std::uint32_t type{};
        if(!game || game!=ActiveGame() || !Read(source,0,type) || type!=1 || !Read(source,4,drop.monsterClass)
            || !Read(source,8,drop.monster) || drop.monster==D2RL::Lifecycle::InvalidUnitId
            || GameFromUnit(source)!=game || Lookup(game,type,drop.monster)!=source) return false;
        drop.level=LevelId(source);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}

bool ReadCreatedGroundItem(void* game,void* unit,RawItem& item) noexcept {
    __try {
        std::uint32_t type{};
        if(!game || !Read(unit,0,type) || type!=4 || !Read(unit,4,item.klass)
            || !Read(unit,8,item.id) || item.id==D2RL::Lifecycle::InvalidUnitId
            || !Read(unit,12,item.mode) || (item.mode!=3 && item.mode!=5)
            || Lookup(game,type,item.id)!=unit) return false;
        ReadItemDetails(unit,item);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}

bool RefreshCreatedItem(const RawItem& original,RawItem& refreshed) noexcept {
    __try {
        auto* game=ActiveGame();
        if(!game) return false;
        auto* unit=Lookup(game,4,original.id);
        return ReadCreatedGroundItem(game,unit,refreshed) && refreshed.id==original.id
            && refreshed.klass==original.klass && refreshed.code==original.code;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}

void EmitExactDrop(const ExactDrop& drop,std::uint64_t death,bool refresh) {
    RawItem item{};
    const bool refreshed=refresh && RefreshCreatedItem(drop.item,item);
    const auto description=DescribeItem(refreshed?item:drop.item);
    const auto monster=std::to_string(drop.monster);
    auto json=Prefix("monster_drop",StateEpoch)+",\"monster_id\":"+monster
        +",\"monster_class\":"+std::to_string(drop.monsterClass)+",\"level\":"+std::to_string(drop.level)
        +",\"exact_source\":true,\"source\":\"treasure_class\",\"item_details\":"
        +Quote(refreshed?"after_creation":"at_creation");
    if(death) json+=",\"death\":"+std::to_string(death);
    Emit(json+description.json+"}","    Confirmed drop from monster "+monster+": "+description.readable);
}

void FlushExactDrops(std::uint32_t monster=D2RL::Lifecycle::InvalidUnitId,std::uint64_t death=0) {
    std::erase_if(PendingExactDrops,[monster,death](const ExactDrop& drop) {
        if(monster!=D2RL::Lifecycle::InvalidUnitId && drop.monster!=monster) return false;
        EmitExactDrop(drop,death,true);
        return true;
    });
}

void* __fastcall TreasureDropHook(void* game,void* source,std::int32_t itemClassId,
        std::int32_t arg4,std::int32_t arg5,std::int32_t arg6) {
    if(!OriginalTreasureDrop) return nullptr;
    ExactDrop drop;
    const auto epoch=Epoch.load();
    const bool observed=Active.load() && InGame.load()
        && reinterpret_cast<std::uintptr_t>(_ReturnAddress())==Context->exeBase+NativeWitnesses::TreasureHelperReturnRva
        && ReadTreasureSource(game,source,drop);
    auto* item=OriginalTreasureDrop(game,source,itemClassId,arg4,arg5,arg6);
    if(!observed || !InGame.load() || Epoch.load()!=epoch || !ReadCreatedGroundItem(game,item,drop.item)) return item;
    try {
        SyncEpoch();
        if(SeenItems.insert(drop.item.id).second) {
            if(Threads && PendingExactDrops.size()<MaxItems) PendingExactDrops.push_back(drop);
            else EmitExactDrop(drop,0,false);
        }
    } catch(...) {LostRecords.fetch_add(1);}
    return item;
}

void RememberItems(const Snapshot& snapshot) {
    for(std::size_t i=0;i<snapshot.count;++i) SeenItems.insert(snapshot.items[i].id);
}

bool Nearby(const RawItem& item,int x,int y) noexcept {
    constexpr long long radius=256;
    const auto dx=static_cast<long long>(item.x)-x;
    const auto dy=static_cast<long long>(item.y)-y;
    return dx>=-radius && dx<=radius && dy>=-radius && dy<=radius
        && dx*dx+dy*dy<=radius*radius;
}

std::string CandidateList(const std::vector<std::uint64_t>& candidates) {
    std::string ids;
    for(const auto id:candidates) {
        if(!ids.empty()) ids+=", ";
        ids+=std::to_string(id);
    }
    return ids;
}

void Observe(const Snapshot& snapshot,const Opening* synchronous,bool openingRoom) {
    for(std::size_t i=0;i<snapshot.count;++i) {
        const auto& item=snapshot.items[i];
        if(!SeenItems.insert(item.id).second) continue;
        if(synchronous) {
            const auto description=DescribeItem(item);
            Emit(Prefix("drop_during_open",StateEpoch)+OpeningFields(*synchronous)+description.json+"}",
                "    Drop during opening #"+std::to_string(synchronous->order)+": "+description.readable);
            continue;
        }
        std::vector<std::uint64_t> openings,deaths;
        for(auto& open:Pending) {
            if(Nearby(item,open.x,open.y)) {
                openings.push_back(open.order);
                ++open.coverage.candidates;
            }
        }
        for(auto& death:PendingDeaths) {
            if(item.areaId==death.event.levelId && Nearby(item,death.x,death.y)) {
                deaths.push_back(death.order);
                ++death.coverage.candidates;
            }
        }
        if(!openingRoom && openings.empty() && deaths.empty()) continue;
        const auto description=DescribeItem(item);
        const auto openingIds=CandidateList(openings);
        const auto deathIds=CandidateList(deaths);
        auto readable="    Ground item (source uncertain): "+description.readable;
        if(!openingIds.empty()) readable+=" | possible openings: "+openingIds;
        if(!deathIds.empty()) readable+=" | possible deaths: "+deathIds;
        Emit(Prefix("later_ground_item",StateEpoch)+",\"candidate_openings\":["+openingIds
            +"],\"candidate_deaths\":["+deathIds+"],\"exact_source\":false"+description.json+"}",std::move(readable));
    }
}

const char* UnitKind(D2RL::Lifecycle::UnitType type) noexcept {
    using enum D2RL::Lifecycle::UnitType;
    switch(type) {
    case Player: return "player";
    case Monster: return "monster";
    case Missile: return "missile";
    case Object: return "object";
    default: return "unit";
    }
}

void __cdecl OnMonsterDeath(const D2RL::PluginContext*,const D2RL::Lifecycle::MonsterDeathEvent* event,void*) noexcept {
    using namespace D2RL::Lifecycle;
    if(!Active.load() || !InGame.load() || !event || !HasMonsterDeathEventField(event,MonsterDeathEventRequiredSize)
        || event->monster.type!=UnitType::Monster) return;
    try {
        SyncEpoch();
        Death death{.order=DeathSequence.fetch_add(1)+1,.tick=GetTickCount64(),.event=*event};
        const bool located=MonsterPositions && DeathRoom(death,true)!=nullptr;
        const bool willScan=located && Threads;
        const auto number=std::to_string(death.order);
        auto json=Prefix("monster_death",StateEpoch)+",\"death\":"+number
            +",\"game_id\":"+std::to_string(event->gameId)+",\"difficulty\":"+std::to_string(event->difficulty)
            +",\"level\":"+std::to_string(event->levelId)+",\"monster_id\":"+std::to_string(event->monster.id)
            +",\"monster_class\":"+std::to_string(event->monster.classId)
            +",\"killer_type\":"+std::to_string(static_cast<std::uint32_t>(event->killer.type))
            +",\"killer_id\":"+std::to_string(event->killer.id)+",\"killer_class\":"+std::to_string(event->killer.classId)
            +",\"position_status\":"+Quote(death.positionStatus)
            +",\"ground_scan\":"+Quote(willScan?"scheduled":(!MonsterPositions?"position_capture_unavailable":
                (!located?"position_unavailable":"game_thread_unavailable")));
        auto readable="  Death #"+number+": monster class "+std::to_string(event->monster.classId)
            +" (unit "+std::to_string(event->monster.id)+") in area "+std::to_string(event->levelId);
        if(located) {
            json+=",\"x\":"+std::to_string(death.x)+",\"y\":"+std::to_string(death.y);
            readable+=" at "+std::to_string(death.x)+", "+std::to_string(death.y);
        } else {
            auto reason=std::string(death.positionStatus);
            std::replace(reason.begin(),reason.end(),'_',' ');
            readable+=" (position unavailable: "+reason+")";
        }
        if(event->killer.type!=UnitType::Invalid && event->killer.id!=InvalidUnitId) {
            readable+=" | direct killer: ";
            readable+=UnitKind(event->killer.type);
            readable+=" "+std::to_string(event->killer.id);
        }
        if(!willScan) readable+=" | ground scan skipped";
        Emit(json+"}",std::move(readable));
        if(!PendingExactDrops.empty()) FlushExactDrops(event->monster.id,death.order);
        if(!willScan) return;
        if(PendingDeaths.size()>=MaxPendingDeaths) {
            if(death.tick-PendingDeaths.front().tick<=WindowMs) LostRecords.fetch_add(1);
            PendingDeaths.pop_front();
        }
        PendingDeaths.push_back(death);
    } catch(...) {LostRecords.fetch_add(1);}
}

std::int32_t __fastcall OperateHook(void* game,void* player,std::uint32_t objectId,std::uint32_t action,bool check) {
    if(!Original) return 0;
    if(!Active.load() || !InGame.load() || InsideObserver) return Original(game,player,objectId,action,check);
    Opening opening{};
    std::unique_ptr<Snapshot> before;
    bool observed{};
    bool eligible{};
    InsideObserver=true;
    try {
        before=std::make_unique<Snapshot>();
        SyncEpoch();
        eligible=GetOpening(game,player,objectId,&opening) && opening.before==0 && !OpenedObjects.contains(objectId);
        observed=eligible && TakeSnapshot(opening.room,before.get(),false);
        if(observed) RememberItems(*before);
    } catch(...) {observed=false;}
    InsideObserver=false;
    // Forward every original argument once. Observation never supplies a seed or invokes drop generation.
    const auto result=Original(game,player,objectId,action,check);
    if(!observed) {
        if(eligible) try {
            Emit(Prefix("sampling_gap",StateEpoch)+OpeningFields(opening)
                +",\"phase\":\"before_open\",\"status\":"+Quote(opening.room?"failed":"skipped")+"}",
                std::string("  Container ground scan ")+(opening.room?"failed":"skipped")
                +" before opening in area "+std::to_string(opening.level));
        } catch(...) {LostRecords.fetch_add(1);}
        return result;
    }
    InsideObserver=true;
    try {
        std::uint32_t after{};
        const bool modeKnown=AfterMode(opening,after);
        Snapshot snapshot;
        const bool captured=TakeSnapshot(opening.room,&snapshot,true,&SeenItems);
        std::size_t newItems{};
        if(captured) for(std::size_t i=0;i<snapshot.count;++i)
            if(!SeenItems.contains(snapshot.items[i].id)) ++newItems;
        if((modeKnown && after!=opening.before) || newItems || (!modeKnown && result!=0)) {
            opening.order=Sequence.fetch_add(1)+1; opening.run=StateEpoch; opening.tick=GetTickCount64();
            OpenedObjects.insert(objectId);
            const bool complete=captured && snapshot.complete && before->complete;
            Emit(Prefix("container_open",StateEpoch)+OpeningFields(opening)
                +",\"mode_before\":"+std::to_string(opening.before)+",\"mode_after\":"+std::to_string(after)
                +",\"result\":"+std::to_string(result)+",\"snapshot_complete\":"
                +(complete?"true":"false")+",\"before_scan\":"+Quote(ScanLabel(ResultOfScan(true,*before)))
                +",\"after_scan\":"+Quote(ScanLabel(ResultOfScan(captured,snapshot)))
                +",\"observed_items\":"+(captured?std::to_string(newItems):"null")
                +",\"delayed_scan\":"+Quote(Threads?"scheduled":"game_thread_unavailable")+"}",
                "  Opening #"+std::to_string(opening.order)+": "+OpeningLabel(opening)+" in area "+std::to_string(opening.level)
                +" at "+std::to_string(opening.x)+", "+std::to_string(opening.y)
                +(complete?" | observed items: "+std::to_string(newItems):" | ground scan incomplete"));
            if(Threads) Pending.push_back(opening);
            if(captured) Observe(snapshot,&opening,true);
        } else {
            Emit(Prefix("container_attempt",StateEpoch)+OpeningFields(opening)
                +",\"result\":"+std::to_string(result)+",\"mode_known\":"+(modeKnown?"true":"false")
                +",\"before_scan\":"+Quote(ScanLabel(ResultOfScan(true,*before)))
                +",\"after_scan\":"+Quote(ScanLabel(ResultOfScan(captured,snapshot)))+"}",
                "  Container attempt "+OpeningLabel(opening)+" in area "+std::to_string(opening.level)+" at "
                +std::to_string(opening.x)+", "+std::to_string(opening.y)
                +" | after scan: "+ScanLabel(ResultOfScan(captured,snapshot)));
        }
    } catch(...) {LostRecords.fetch_add(1);}
    InsideObserver=false;
    return result;
}
void* SamplingRoom(const Opening& open) noexcept {
    __try {
        void* player=Lookup(open.game,0,open.player);
        std::uint32_t type{};
        if(!Read(player,0,type) || type!=0 || GameFromUnit(player)!=open.game) return nullptr;
        void* object=Lookup(open.game,2,open.object);
        if(Read(object,0,type) && type==2) return Room(object);
        return Room(player);
    } __except(EXCEPTION_EXECUTE_HANDLER) {return nullptr;}
}

ScanStatus ScanRoom(void* room,bool openingRoom,std::unordered_map<void*,ScanStatus>& scans) {
    if(!room) return ScanStatus::Skipped;
    const auto [entry,fresh]=scans.try_emplace(room,ScanStatus::Failed);
    if(fresh) {
        Snapshot snapshot;
        const bool captured=TakeSnapshot(room,&snapshot,true,&SeenItems);
        entry->second=ResultOfScan(captured,snapshot);
        if(captured) Observe(snapshot,nullptr,openingRoom);
    }
    return entry->second;
}

void ReportWindow(const char* source,std::uint64_t number,const Coverage& coverage) {
    const bool gaps=coverage.failed || coverage.incomplete || coverage.skipped;
    const char* status=coverage.complete && !gaps?"complete":
        (coverage.complete || coverage.incomplete?"incomplete":(coverage.failed?"failed":"skipped"));
    const auto id=std::to_string(number);
    const auto items=std::to_string(coverage.candidates);
    auto line=std::string("    Ground scan for ")+source+" #"+id+" | "+status+" | candidate items: "+items;
    if(coverage.complete && !gaps && coverage.candidates) line.clear();
    Emit(Prefix("sampling_window",StateEpoch)+",\""+source+"\":"+id+",\"status\":"+Quote(status)
        +",\"complete_scans\":"+std::to_string(coverage.complete)
        +",\"incomplete_scans\":"+std::to_string(coverage.incomplete)
        +",\"failed_scans\":"+std::to_string(coverage.failed)
        +",\"skipped_scans\":"+std::to_string(coverage.skipped)+",\"candidate_items\":"+items+"}",std::move(line));
}

void __cdecl Sample(const D2RL::PluginContext*,void*) noexcept {
    try {
        if(Active.load() && InGame.load()) {
            SyncEpoch(); const auto now=GetTickCount64();
            if(!PendingExactDrops.empty()) FlushExactDrops();
            std::erase_if(Pending,[now](const Opening& open) {
                if(now-open.tick<=WindowMs) return false;
                ReportWindow("opening",open.order,open.coverage);
                return true;
            });
            while(!PendingDeaths.empty() && now-PendingDeaths.front().tick>WindowMs) {
                const auto& death=PendingDeaths.front();
                ReportWindow("death",death.order,death.coverage);
                PendingDeaths.pop_front();
            }
            std::unordered_map<void*,ScanStatus> scans;
            for(auto& open:Pending) open.coverage.Add(ScanRoom(SamplingRoom(open),true,scans));
            for(auto& death:PendingDeaths) death.coverage.Add(ScanRoom(DeathRoom(death,false),false,scans));
            if(MonsterPositions && now-LastBaseline>=BaselineIntervalMs) {
                LastBaseline=now;
                auto* room=PlayerRoom();
                Snapshot baseline;
                const bool matchDeaths=!PendingDeaths.empty();
                if(room && !scans.contains(room) && TakeSnapshot(room,&baseline,matchDeaths,&SeenItems)) {
                    if(matchDeaths) Observe(baseline,nullptr,false);
                    else RememberItems(baseline);
                }
            }
        }
    } catch(...) {LostRecords.fetch_add(1);}
    Queued.store(false);
}

bool IsSessionStem(std::wstring_view stem) noexcept {
    constexpr std::wstring_view pattern=L"drops-0000-00-00_00-00-00.000Z_pid-";
    const auto isDigit=[](wchar_t ch) {return ch>=L'0' && ch<=L'9';};
    if(stem.size()<=pattern.size()) return false;
    for(std::size_t i=0;i<pattern.size();++i) {
        if(pattern[i]==L'0'?!isDigit(stem[i]):pattern[i]!=stem[i]) return false;
    }
    const auto pid=stem.substr(pattern.size());
    return std::all_of(pid.begin(),pid.end(),isDigit);
}

void PruneLogs(const std::filesystem::path& current) {
    std::map<std::wstring,std::vector<std::filesystem::path>,std::greater<>> sessions;
    for(const auto& file:std::filesystem::directory_iterator(OutputDirectory)) {
        if(!std::filesystem::is_regular_file(file.symlink_status())) continue;
        const auto& path=file.path();
        const auto extension=path.extension();
        if(extension!=L".jsonl" && extension!=L".txt") continue;
        auto stem=path.stem().wstring();
        if(IsSessionStem(stem)) sessions[std::move(stem)].push_back(path);
    }
    std::size_t kept=1;
    for(const auto& [stem,files]:sessions) {
        if(stem==current.native()) continue;
        if(kept<SessionsToKeep) {++kept;continue;}
        for(const auto& path:files) {
            std::error_code error;
            std::filesystem::remove(path,error);
            if(error) Context->LogError("Drop Logger could not remove an older session log.");
        }
    }
}

struct SessionFiles {
    std::filesystem::path stem;
    std::ofstream json, readable;
};

DWORD WINAPI WriteAndPoll(void* data) {
    const std::unique_ptr<SessionFiles> files(static_cast<SessionFiles*>(data));
    try {PruneLogs(files->stem);}
    catch(...) {Context->LogError("Drop Logger could not prune older session logs.");}
    while(true) {
        const bool stop=WaitForSingleObject(StopEvent,25)==WAIT_OBJECT_0;
        std::deque<LogRecord> batch;
        {std::lock_guard lock(QueueMutex);batch.swap(Queue);}
        for(const auto& record:batch) {
            files->json<<record.json<<'\n';
            if(!record.readable.empty()) files->readable<<record.readable<<'\n';
        }
        if(!batch.empty()) {files->json.flush();files->readable.flush();}
        if(stop) break;
        if(Threads && Active.load() && InGame.load() && !Queued.exchange(true)) {
            if(Threads->runOnGameThread(Context,Sample,nullptr)!=D2RL::Threads::Result::Success) Queued.store(false);
        }
    }
    return 0;
}
std::string GameHeading(std::string_view characterName) {
    constexpr std::array names{"Normal","Nightmare","Hell"};
    return "\nGame "+std::to_string(GameNumber)+" | "+(GameDifficulty?names[*GameDifficulty]:"Difficulty unknown")
        +" | "+(characterName.empty()?"Character unknown":SingleLine(characterName));
}
void __cdecl OnGameplay(const D2RL::PluginContext*,const D2RL::Lifecycle::GameplayEvent* event,void*) noexcept {
    if(!event || event->structSize<D2RL::Lifecycle::GameplayEventRequiredSize) return;
    try {
        if(event->kind==D2RL::Lifecycle::GameplayEventKind::GameJoined) {
            InGame.store(false); Epoch.store(event->sessionGeneration); Sequence.store(0); DeathSequence.store(0);
            LocalPlayerId.store(event->playerId); Queued.store(false);
            GameHeadingPending=true;
            GameDifficulty=ReadGameDifficulty();
            const auto gameNumber=std::to_string(++GameNumber);
            const bool selector=GetModuleHandleW(L"d2rl-map-seed-selector.dll")!=nullptr;
            Emit(Prefix("game_joined",event->sessionGeneration)+",\"game_number\":"+gameNumber
                +",\"difficulty\":"+(GameDifficulty?std::to_string(*GameDifficulty):"null")
                +",\"player_id\":"+std::to_string(event->playerId)
                +",\"seed_selector_module_loaded\":"+(selector?"true":"false")+"}",{});
            InGame.store(true);
        } else if(event->kind==D2RL::Lifecycle::GameplayEventKind::LocalPlayerReady) {
            Epoch.store(event->sessionGeneration); LocalPlayerId.store(event->playerId); InGame.store(true);
            if(!GameHeadingPending) return;
            if(const auto difficulty=ReadGameDifficulty()) GameDifficulty=difficulty;
            char name[NativeWitnesses::CharacterNameCapacity]{};
            std::string_view characterName;
            if(!ReadCharacterName(event->playerId,name) || !BoundedText(name,characterName)) characterName={};
            GameHeadingPending=!Emit(Prefix("game_ready",event->sessionGeneration)
                +",\"game_number\":"+std::to_string(GameNumber)
                +",\"difficulty\":"+(GameDifficulty?std::to_string(*GameDifficulty):"null")
                +",\"player_id\":"+std::to_string(event->playerId)
                +",\"character_name\":"+(characterName.empty()?"null":Quote(characterName))+"}",
                GameHeading(characterName));
        } else if(event->kind==D2RL::Lifecycle::GameplayEventKind::GameLeft) {
            InGame.store(false); Queued.store(false);
            LocalPlayerId.store(D2RL::Lifecycle::InvalidUnitId);
            const auto openings=std::to_string(Sequence.load());
            const auto deaths=std::to_string(DeathSequence.load());
            const auto lost=std::to_string(LostRecords.load());
            const auto gameNumber=std::to_string(GameNumber);
            Emit(Prefix("game_left",event->sessionGeneration)+",\"game_number\":"+gameNumber
                +",\"openings\":"+openings
                +",\"monster_deaths\":"+deaths+",\"lost_records\":"+lost+"}",
                (GameHeadingPending?GameHeading({})+'\n':std::string{})+"Game "+gameNumber+" ended | "+openings
                +" openings | "+deaths+" monster deaths | "+lost+" records lost this session");
            GameHeadingPending=false;
        }
    } catch(...) {LostRecords.fetch_add(1);}
}
template<class Fn> Fn At(std::uintptr_t rva) {return reinterpret_cast<Fn>(Context->exeBase+rva);}

void ReportNativeIssue(std::vector<LogRecord>& records,const char* feature,const char* issue,
        std::uintptr_t rva,std::span<const std::uint8_t> bytes) {
    using namespace D2RL::Diagnostics;
    const HookQuery query{.structSize=HookQuerySize,.rva=rva,.expected=bytes.data(),
        .expectedSize=static_cast<std::uint32_t>(bytes.size())};
    HookStatus status{.structSize=HookStatusSize};
    const char* state="unavailable";
    if(Diagnostics && Diagnostics->queryHookStatus(Context,&query,&status)==Result::Success) {
        switch(status.state) {
        case ModificationState::Tracked: state="tracked"; break;
        case ModificationState::Untracked: state="untracked"; break;
        case ModificationState::Unchanged: state="unchanged"; break;
        }
    }
    status.ownerPluginId[sizeof(status.ownerPluginId)-1]=0;
    char address[24];
    std::snprintf(address,sizeof(address),"0x%llX",static_cast<unsigned long long>(rva));
    auto line=std::string("Capture diagnostic | ")+feature+" | "+issue+" at RVA "+address+" | "+state;
    if(status.ownerPluginId[0]) line+=" | owner "+SingleLine(status.ownerPluginId);
    else if(status.ownerCount) line+=" | "+std::to_string(status.ownerCount)+" owners";
    Context->LogInfo(line.c_str());
    records.push_back({Prefix("capture_diagnostic",0)+",\"feature\":"+Quote(feature)
        +",\"issue\":"+Quote(issue)+",\"rva\":"+Quote(address)
        +",\"modification\":"+Quote(state)+",\"owner_plugin_id\":"+Quote(status.ownerPluginId)
        +",\"owner_count\":"+std::to_string(status.ownerCount)+"}",std::move(line)});
}

bool Check(std::vector<LogRecord>& records,const char* feature,
        std::uintptr_t rva,std::span<const std::uint8_t> bytes) {
    if(Context->CheckExpectedBytes(rva,bytes.data(),static_cast<std::uint32_t>(bytes.size()))) {
        D2RL::LogInfoF(Context,"Drop Logger native witness RVA 0x%llX: match",static_cast<unsigned long long>(rva));
        return true;
    }
    ReportNativeIssue(records,feature,"witness_mismatch",rva,bytes);
    return false;
}

template<std::size_t Size>
bool CheckCoreWitness(HMODULE core,std::uintptr_t rva,const std::array<std::uint8_t,Size>& expected) noexcept {
    std::array<std::uint8_t,Size> actual{};
    return Read(core,rva,actual) && actual==expected;
}

std::uintptr_t JumpTarget(std::uintptr_t instruction,const std::uint8_t* bytes) noexcept {
    if(bytes[0]!=0xE9) return 0;
    std::int32_t displacement{};
    std::memcpy(&displacement,bytes+1,sizeof(displacement));
    return instruction+5+static_cast<std::uintptr_t>(displacement);
}

bool MatchesChestLockLayout() noexcept {
    using namespace NativeWitnesses;
    const auto getterAddress=Context->exeBase+ChestInteractGetterRva;
    std::array<std::uint8_t,ChestInteractGetterBytes.size()> getter{};
    if(!Read(reinterpret_cast<const void*>(getterAddress),0,getter)) return false;
    if(getter==ChestInteractGetterBytes) return true;
    constexpr auto tail=ChestInteractGetterTail;
    if(!std::equal(getter.begin(),getter.begin()+tail,ChestInteractGetterBytes.begin())
        || getter[tail+5]!=0x90 || getter[tail+6]!=0x90 || getter[tail+7]!=0x90
        || !std::equal(getter.begin()+tail+8,getter.end(),ChestInteractGetterBytes.begin()+tail+8)) return false;
    const auto relayAddress=JumpTarget(getterAddress+tail,getter.data()+tail);
    std::array<std::uint8_t,ChestInteractGetterRelay.size()+5> relay{};
    return Read(reinterpret_cast<const void*>(relayAddress),0,relay)
        && std::equal(ChestInteractGetterRelay.begin(),ChestInteractGetterRelay.end(),relay.begin())
        && JumpTarget(relayAddress+ChestInteractGetterRelay.size(),relay.data()+ChestInteractGetterRelay.size())
            ==getterAddress+tail+8;
}

void Cleanup() noexcept {
    Active.store(false); InGame.store(false);
    if(DeathListener) {
        (void)Lifecycle->unregisterMonsterDeathListener(Context,DeathListener); DeathListener=0;
    }
    if(Lifecycle && Context) for(auto& listener:Listeners) if(listener) {
        (void)Lifecycle->unregisterGameplayEventListener(Context,listener); listener=0;
    }
    if(StopEvent) SetEvent(StopEvent);
    if(Worker) {WaitForSingleObject(Worker,INFINITE);CloseHandle(Worker);Worker=nullptr;}
    if(StopEvent) {CloseHandle(StopEvent);StopEvent=nullptr;}
}
} // namespace

D2RL_PLUGIN_EXPORT const D2RL::PluginInfo* __cdecl D2RLoaderGetPluginInfo() noexcept {return &Info;}
D2RL_PLUGIN_EXPORT bool __cdecl D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept {
    Context=context;
    try {
        if(!D2RL::HasContext(Context) || Context->abiVersion!=D2RL_PLUGIN_ABI_VERSION) return false;
        if(!Context->GetApi() || !Context->pluginLogPath) {
            Context->LogError("Drop Logger requires the plugin API and log path."); return false;
        }
        if(Context->QueryService(&Threads)!=D2RL::ServiceQueryResult::Success
            || !D2RL::HasThreadServiceField(Threads,D2RL::ThreadServiceRequiredSize)
            || !Threads->runOnGameThread) {
            Threads=nullptr;
            Context->LogInfo("Drop Logger delayed scans are unavailable without the game-thread scheduling service.");
        }
        if(Context->QueryService(&Lifecycle)!=D2RL::ServiceQueryResult::Success
            || !D2RL::HasLifecycleServiceField(Lifecycle,offsetof(D2RL::LifecycleService,registerMonsterDeathListener))
            || !Lifecycle->registerGameplayEventListener || !Lifecycle->unregisterGameplayEventListener) {
            Context->LogError("Drop Logger requires the gameplay lifecycle service."); return false;
        }
        if(Context->QueryService(&Localization)!=D2RL::ServiceQueryResult::Success
            || !D2RL::HasLocalizationServiceField(Localization,D2RL::LocalizationServiceRequiredSize)) Localization=nullptr;
        if(Context->QueryService(&Diagnostics)!=D2RL::ServiceQueryResult::Success
            || !D2RL::HasDiagnosticsServiceField(Diagnostics,D2RL::DiagnosticsServiceRequiredSize)
            || !Diagnostics->queryHookStatus) Diagnostics=nullptr;
        SYSTEMTIME time{};GetSystemTime(&time);char stamp[80];
        std::snprintf(stamp,sizeof(stamp),"%04u-%02u-%02u_%02u-%02u-%02u.%03uZ_pid-%lu",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds,GetCurrentProcessId());
        Capture=stamp;
        std::vector<LogRecord> startupIssues;
        const auto core=GetModuleHandleW(L"D2RCore.dll");
        ReadStat=core?reinterpret_cast<StatFn>(GetProcAddress(core,"ReadWideUnitStat")):nullptr;
        LevelId=core?reinterpret_cast<D2RL::Reimplementations::GetLevelIdFromUnitFn>(GetProcAddress(core,D2RL::Reimplementations::GetLevelIdFromUnitInfo.name)):nullptr;
        const auto version=core?reinterpret_cast<D2RL::CoreExports::VersionFn>(
            GetProcAddress(core,D2RL::CoreExports::VersionInfo.name)):nullptr;
        const auto* loaderVersion=version?version():nullptr;
        const auto* gameBuild=Context->buildVersion;
        if(!loaderVersion || !loaderVersion[0]) loaderVersion="unknown";
        if(!gameBuild || !gameBuild[0]) gameBuild="unknown";
        using namespace NativeWitnesses;
        bool groundMatches=ReadStat && LevelId;
        if(!groundMatches) startupIssues.push_back({
            Prefix("capture_diagnostic",0)+",\"feature\":\"ground\",\"issue\":\"read_exports_unavailable\"}",
            "Capture diagnostic | ground | read-only stat/level exports unavailable"});
        bool containerMatches=true;
        for(const auto& witness:Witnesses) {
            const auto rva=witness.rva;
            const bool containerOnly=rva==ObjectDispatcherRva || rva==ObjectRecordRva
                || rva==Operation4Rva || rva==Operation19Rva || rva==Operation20Rva;
            const bool matches=Check(startupIssues,containerOnly?"containers":"ground",rva,witness.bytes);
            if(containerOnly) containerMatches=containerMatches && matches;
            else groundMatches=groundMatches && matches;
        }
        const bool activeGameMatches=Check(startupIssues,"active_game",ActiveGameStoreRva,ActiveGameStoreBytes);
        const bool gameMatches=groundMatches && activeGameMatches;
        GameDifficultyLayout=activeGameMatches && CheckCoreWitness(core,CoreDifficultyReadRva,CoreDifficultyReadBytes);
        ItemQualityLayout=CheckCoreWitness(core,CoreItemQualityReadRva,CoreItemQualityReadBytes)
            && Check(startupIssues,"item_quality",ItemDataLayoutRva,ItemDataLayoutBytes);
        const bool characterNameLayout=CheckCoreWitness(core,CoreCharacterNameReadRva,CoreCharacterNameReadBytes)
            && Check(startupIssues,"character_name",GetLocalDataContextRva,GetLocalDataContextBytes)
            && Check(startupIssues,"character_name",GetLocalPlayerRva,GetLocalPlayerBytes);
        GetLocalDataContext=characterNameLayout?At<GetLocalDataContextFn>(GetLocalDataContextRva):nullptr;
        GetLocalPlayer=characterNameLayout?At<GetLocalPlayerFn>(GetLocalPlayerRva):nullptr;
        bool treasureMatches=gameMatches;
        if(gameMatches) for(const auto& witness:ExactMonsterDropWitnesses) {
            const bool matches=Check(startupIssues,"monster_drops",witness.rva,witness.bytes);
            treasureMatches=treasureMatches && matches;
        }
        Room=At<UnitFn>(GetUnitRoomRva);First=At<UnitFn>(RoomFirstUnitRva);Next=At<UnitFn>(NextUnitRva);
        Neighbours=At<NeighboursFn>(RoomNeighboursRva);Lookup=At<LookupFn>(ServerUnitRva);
        GameFromUnit=At<UnitFn>(GetGameFromUnitRva);ObjectRecord=At<UnitFn>(ObjectRecordRva);
        ItemCode=At<CodeFn>(GetItemCodeRva);CoordX=At<CoordFn>(GetXRva);CoordY=At<CoordFn>(GetYRva);
        OutputDirectory=std::filesystem::path(Context->pluginLogPath).parent_path()/L"drop logs";
        std::filesystem::create_directories(OutputDirectory);
        GameNumber=0;
        GameHeadingPending=false;
        constexpr std::array kinds{D2RL::Lifecycle::GameplayEventKind::GameJoined,D2RL::Lifecycle::GameplayEventKind::LocalPlayerReady,D2RL::Lifecycle::GameplayEventKind::GameLeft};
        for(std::size_t i=0;i<kinds.size();++i) {
            const D2RL::Lifecycle::GameplayEventListener listener{.structSize=D2RL::Lifecycle::GameplayEventListenerSize,.kind=kinds[i],.callback=OnGameplay};
            if(Lifecycle->registerGameplayEventListener(Context,&listener,&Listeners[i])!=D2RL::Lifecycle::Result::Success) {
                Context->LogError("Drop Logger could not register its gameplay listener."); Cleanup();return false;
            }
        }
        if(D2RL::HasLifecycleServiceField(Lifecycle,D2RL::LifecycleServiceRequiredSize)
            && Lifecycle->registerMonsterDeathListener && Lifecycle->unregisterMonsterDeathListener) {
            const D2RL::Lifecycle::MonsterDeathListener listener{
                .structSize=D2RL::Lifecycle::MonsterDeathListenerSize,.callback=OnMonsterDeath};
            if(Lifecycle->registerMonsterDeathListener(Context,&listener,&DeathListener)==D2RL::Lifecycle::Result::Success) {
                MonsterPositions=gameMatches;
            } else {
                DeathListener=0;
            }
        }
        if(!DeathListener) Context->LogInfo("Monster-death events unavailable.");
        else if(!MonsterPositions) Context->LogInfo("Monster position validation failed. Deaths will be logged without drop candidates.");

        auto files=std::make_unique<SessionFiles>();
        files->stem="drops-"+Capture;
        files->json.open(OutputDirectory/(files->stem.native()+L".jsonl"),std::ios::binary|std::ios::app);
        files->readable.open(OutputDirectory/(files->stem.native()+L".txt"),std::ios::binary|std::ios::app);
        if(!files->json || !files->readable) {
            Context->LogError("Drop Logger could not open its session output files."); Cleanup();return false;
        }
        StopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!StopEvent) {Context->LogError("Drop Logger could not create its stop event."); Cleanup();return false;}
        bool containersEnabled=false;
        if(groundMatches && containerMatches) {
            ChestFlagsLayout=Check(startupIssues,"chest_rule",ChestFlagsGetterRva,ChestFlagsGetterBytes);
            ChestLockLayout=MatchesChestLockLayout();
            if(!ChestLockLayout) ReportNativeIssue(startupIssues,"chest_lock","witness_mismatch",
                ChestInteractGetterRva,ChestInteractGetterBytes);
            const bool installed=Context->InstallInlineHook(ObjectDispatcherRva,ObjectDispatcherBytes.data(),15,
                reinterpret_cast<void*>(OperateHook),reinterpret_cast<void**>(&Original));
            if(installed && !Original) {
                Context->LogError("Drop Logger hook installation returned no original dispatcher.");
                Cleanup(); return false;
            }
            containersEnabled=installed;
            if(!installed) ReportNativeIssue(startupIssues,"containers","install_rejected",ObjectDispatcherRva,ObjectDispatcherBytes);
        }
        bool exactDropsEnabled=false;
        if(treasureMatches) {
            const bool installed=Context->InstallInlineHook(TreasureHelperRva,TreasureHelperEntryBytes.data(),
                TreasureHelperPrologueSize,TreasureDropHook,&OriginalTreasureDrop);
            if(installed && !OriginalTreasureDrop) {
                Context->LogError("Drop Logger Treasure Class hook installation returned no original function.");
                Cleanup(); return false;
            }
            exactDropsEnabled=installed;
            if(!installed) ReportNativeIssue(startupIssues,"monster_drops","install_rejected",TreasureHelperRva,TreasureHelperEntryBytes);
        }
        const bool breadcrumbsEnabled=PublishBreadcrumbs();
        const auto enabled=[](bool value) {return value?"enabled":"unavailable";};
        const auto jsonBool=[](bool value) {return value?"true":"false";};
        const auto scope=Context->loadScope==D2RL::LoadScope::Mod?"mod":"global";
        Emit(Prefix("capture_start",0)+",\"logger_version\":"+Quote(Info.version)
            +",\"schema_version\":4,\"game_build\":"+Quote(gameBuild)+",\"loader_version\":"+Quote(loaderVersion)
            +",\"load_scope\":"+Quote(scope)+",\"active_mod\":"+Quote(Context->activeMod?Context->activeMod:"")
            +",\"game_difficulty_enabled\":"+jsonBool(GameDifficultyLayout)
            +",\"character_name_enabled\":"+jsonBool(characterNameLayout)
            +",\"item_quality_enabled\":"+jsonBool(ItemQualityLayout)
            +",\"delayed_window_ms\":"+std::to_string(WindowMs)+",\"seed_writes\":false"
            +",\"containers_enabled\":"+jsonBool(containersEnabled)
            +",\"monster_deaths_enabled\":"+jsonBool(DeathListener!=0)
            +",\"monster_positions_enabled\":"+jsonBool(MonsterPositions)
            +",\"exact_monster_drops_enabled\":"+jsonBool(exactDropsEnabled)
            +",\"delayed_scans_enabled\":"+jsonBool(Threads && (containersEnabled || MonsterPositions))
            +",\"breadcrumbs_enabled\":"+jsonBool(breadcrumbsEnabled)+"}",
            std::string(Info.name)+" "+Info.version+" | Session "+Capture+" (UTC)\n"
            +"D2R "+SingleLine(gameBuild)+" | D2RLoader "+SingleLine(loaderVersion)+" | Scope: "+scope
            +(Context->activeMod && Context->activeMod[0]?" ("+SingleLine(Context->activeMod)+")":"")+"\n"
            +"Containers: "+enabled(containersEnabled)+" | Monster deaths: "+enabled(DeathListener!=0)
            +" | Monster positions: "+enabled(MonsterPositions)
            +" | Confirmed monster drops: "+enabled(exactDropsEnabled)
            +" | Delayed scans: "+enabled(Threads && (containersEnabled || MonsterPositions))
            +" | Breadcrumbs: "+enabled(breadcrumbsEnabled));
        for(auto& record:startupIssues) Emit(std::move(record.json),std::move(record.readable));
        auto* workerFiles=files.release();
        Worker=CreateThread(nullptr,0,WriteAndPoll,workerFiles,0,nullptr);
        if(!Worker) {
            delete workerFiles;
            Context->LogError("Drop Logger could not create its output worker."); Cleanup();return false;
        }
        Active.store(true);
        Context->LogInfo("Drop Logger active. Session logs are in d2rloader/logs/drop logs.");
        return true;
    } catch(...) {if(Context) Context->LogError("Drop Logger initialization raised an exception."); Cleanup();return false;}
}
D2RL_PLUGIN_EXPORT void __cdecl D2RLoaderUnloadPlugin() noexcept {Cleanup();}
