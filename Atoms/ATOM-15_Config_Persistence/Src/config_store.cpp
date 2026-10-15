#include "atom15_config/config_store.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <utility>
#include "nlohmann/json.hpp"
#include "nlohmann/json-schema.hpp"
#include "config_schema_embed.hpp"
namespace marshland::atom15 {
namespace {
using nlohmann::json;
// nlohmann parses non-negative integers above INT64_MAX as number_unsigned; the
// previous hand-written parser rejected those as out of range, so treat them as
// non-integers here as well.
bool integer(const json& v) {
    if(v.is_number_unsigned()) return v.get<std::uint64_t>()<=static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return v.is_number_integer();
}
std::int64_t intValue(const json& v) { return v.is_number_unsigned()?static_cast<std::int64_t>(v.get<std::uint64_t>()):v.get<std::int64_t>(); }
bool number(const json& v) { return v.is_number(); }
const json* member(const json* v, std::string_view key) { if(!v||!v->is_object()) return nullptr; const auto i=v->find(key); return i==v->end()?nullptr:&*i; }
json* member(json* v, std::string_view key) { if(!v||!v->is_object()) return nullptr; const auto i=v->find(key); return i==v->end()?nullptr:&*i; }
const json* findPath(const json& root, std::string_view key) { const json* v=&root; std::size_t b=0; while(b<key.size()) { const auto e=key.find('.',b); v=member(v,key.substr(b,e==std::string_view::npos?key.size()-b:e-b)); if(!v) return nullptr; if(e==std::string_view::npos) break; b=e+1; } return v; }
json* mutablePath(json& root, std::string_view key) { json* v=&root; std::size_t b=0; while(b<key.size()) { const auto e=key.find('.',b); v=member(v,key.substr(b,e==std::string_view::npos?key.size()-b:e-b)); if(!v) return nullptr; if(e==std::string_view::npos) break; b=e+1; } return v; }
void diagnostic(std::vector<ConfigDiagnostic>& d, std::string key, std::string message) { d.push_back({std::move(key),std::move(message),false}); }
// 编译期嵌入的 Rule/ConfigSchema.schema.json（见上层 CMakeLists 的嵌入段）。
// validator 首次使用时编译一次并缓存；校验本身是 const 的，可在锁内复用。
nlohmann::json_schema::json_validator& schemaValidator() {
    static const auto validator=[] { return std::make_unique<nlohmann::json_schema::json_validator>(json::parse(kConfigSchemaJson)); }();
    return *validator;
}
class SchemaErrorCollector final : public nlohmann::json_schema::error_handler {
public:
    void error(const json::json_pointer& ptr, const json&, const std::string& message) override {
        diagnostics_.push_back({ptr.empty()?std::string():ptr.to_string(),message,false});
    }
    std::vector<ConfigDiagnostic> diagnostics_;
};
// JSON Schema 表达不了、且不与 Schema 冲突的跨字段/跨元素语义检查。
void validateSemantics(const json& root, std::vector<ConfigDiagnostic>& d) {
    const auto motors=member(&root,"motor");
    if(motors&&motors->is_array()) {
        std::vector<std::int64_t> ids; std::vector<std::string> nodes;
        for(std::size_t i=0;i<motors->size();++i) {
            const auto& m=(*motors)[i]; if(!m.is_object()) continue;
            const auto p="motor["+std::to_string(i)+"]";
            const auto id=member(&m,"id");
            if(id&&integer(*id)) { if(std::find(ids.begin(),ids.end(),intValue(*id))!=ids.end()) diagnostic(d,p+".id","duplicate motor id"); ids.push_back(intValue(*id)); }
            const auto node=member(&m,"node");
            if(node&&node->is_string()) { const auto& s=node->get_ref<const std::string&>(); if(std::find(nodes.begin(),nodes.end(),s)!=nodes.end()) diagnostic(d,p+".node","duplicate motor node"); nodes.push_back(s); }
        }
    }
    if(const auto sbus=member(&root,"sbus"); sbus&&sbus->is_object()) {
        const auto min=member(sbus,"valid_min"); const auto max=member(sbus,"valid_max");
        if(min&&max&&integer(*min)&&integer(*max)&&intValue(*min)>intValue(*max)) diagnostic(d,"sbus.valid_min","must not exceed valid_max");
    }
    if(const auto servo=member(&root,"servo"); servo&&servo->is_object()) {
        if(const auto model=member(servo,"model"); model&&model->is_object()) {
            const auto lo=member(model,"pulse_min_us"); const auto hi=member(model,"pulse_max_us");
            if(lo&&hi&&integer(*lo)&&integer(*hi)&&intValue(*hi)<=intValue(*lo)) diagnostic(d,"servo.model.pulse_max_us","must exceed pulse_min_us");
            const auto angles=member(model,"angle_range_deg");
            if(angles&&angles->is_array()&&angles->size()==2&&integer((*angles)[0])&&integer((*angles)[1])&&intValue((*angles)[1])<=intValue((*angles)[0])) diagnostic(d,"servo.model.angle_range_deg","must be an increasing angle range");
        }
        if(const auto pca=member(servo,"pca9685"); pca&&pca->is_object()) {
            const auto range=member(pca,"channel_range");
            if(range&&range->is_array()&&range->size()==2&&integer((*range)[0])&&integer((*range)[1])) {
                const auto low=intValue((*range)[0]); const auto high=intValue((*range)[1]);
                if(low<0||high<low||high>=16) diagnostic(d,"servo.pca9685.channel_range","must be ordered within 0..15");
                if(low>4||high<5) diagnostic(d,"servo.pca9685.channel_range","must include configured FEED/FIRE channels 4 and 5");
            }
            if(const auto freq=member(pca,"pwm_frequency_hz"); freq&&integer(*freq)) {
                const double hz=static_cast<double>(intValue(*freq));
                // Match the existing ATOM-07 prescale calculation without depending on its private headers.
                const double prescale=std::round(25000000.0/(4096.0*hz))-1.0;
                if(hz<24.0||hz>1526.0||prescale<3.0||prescale>255.0)
                    diagnostic(d,"servo.pca9685.pwm_frequency_hz","unsupported PWM frequency or prescale outside 3..255");
            }
        }
    }
}
bool validate(const json& root, std::vector<ConfigDiagnostic>& d) {
    try {
        SchemaErrorCollector collector;
        schemaValidator().validate(root,collector);
        for(auto& entry:collector.diagnostics_) d.push_back(std::move(entry));
    } catch(const std::exception& e) {
        diagnostic(d,"",std::string("schema validation failed internally: ")+e.what());
    }
    validateSemantics(root,d);
    return d.empty();
}
// The library parser silently keeps the last value on duplicate object keys;
// the previous hand-written parser rejected them, so track keys per object via
// the parse callback to preserve that diagnostic.
class DuplicateKeyTracker final {
public:
    bool operator()(int, json::parse_event_t event, json& value) {
        if(event==json::parse_event_t::object_start) stack_.emplace_back();
        else if(event==json::parse_event_t::object_end) { if(!stack_.empty()) stack_.pop_back(); }
        else if(event==json::parse_event_t::key&&!stack_.empty()) {
            const auto& key=value.get_ref<const std::string&>();
            if(!stack_.back().insert(key).second&&duplicate_.empty()) duplicate_=key;
        }
        return true;
    }
    const std::string& duplicate() const noexcept { return duplicate_; }
private:
    std::vector<std::set<std::string>> stack_; std::string duplicate_;
};
json toJson(const ConfigValue& value) { return std::visit([](const auto& x)->json { return json(x); },value); }
bool compatible(const json& current,const ConfigValue& value) { return std::visit([&](const auto& x) { using T=std::decay_t<decltype(x)>; if constexpr(std::is_same_v<T,std::int64_t>) return integer(current); else if constexpr(std::is_same_v<T,double>) return number(current); else if constexpr(std::is_same_v<T,bool>) return current.is_boolean(); else return current.is_string(); },value); }
bool allowed(UpdateSource source,UpdateMode mode) noexcept { return source==UpdateSource::WEB&&(mode==UpdateMode::MANUAL||mode==UpdateMode::DEBUG); }
const char* errorName(ConfigError e) noexcept { switch(e) { case ConfigError::OK:return "OK"; case ConfigError::FILE_NOT_FOUND:return "FILE_NOT_FOUND"; case ConfigError::FILE_IO:return "FILE_IO"; case ConfigError::INVALID_JSON:return "INVALID_JSON"; case ConfigError::INVALID_SCHEMA:return "INVALID_SCHEMA"; case ConfigError::NOT_LOADED:return "NOT_LOADED"; case ConfigError::MISSING:return "MISSING"; case ConfigError::INVALID_TYPE:return "INVALID_TYPE"; case ConfigError::OUT_OF_RANGE:return "OUT_OF_RANGE"; case ConfigError::NOT_AUTHORIZED:return "NOT_AUTHORIZED"; case ConfigError::PERSISTENCE_FAILED:return "PERSISTENCE_FAILED"; case ConfigError::LIMIT_REACHED:return "LIMIT_REACHED"; case ConfigError::NOT_FOUND:return "NOT_FOUND"; } return "UNKNOWN"; }
std::string reservedKey(std::uint32_t index) { return std::string("reserved_")+(index<10?"00":index<100?"0":"")+std::to_string(index); }
} // namespace
struct ConfigStore::Impl final { mutable std::shared_mutex mutex; std::shared_ptr<json> snapshot; std::filesystem::path file_path; std::vector<ConfigDiagnostic> diagnostics; ChangeCallback callback; std::uint64_t version{0}; };
ConfigStore::ConfigStore(ChangeCallback callback):impl_(std::make_unique<Impl>()) { impl_->callback=std::move(callback); }
ConfigStore::~ConfigStore()=default;
ConfigLoadResult ConfigStore::load(const std::filesystem::path& file) { ConfigLoadResult r; std::ifstream input(file,std::ios::binary); if(!input) { r.error=std::filesystem::exists(file)?ConfigError::FILE_IO:ConfigError::FILE_NOT_FOUND; r.diagnostics.push_back({file.string(),errorName(r.error),false}); return r; } std::ostringstream b; b<<input.rdbuf(); json parsed; DuplicateKeyTracker tracker; try { parsed=json::parse(b.str(),std::ref(tracker)); } catch(const json::exception& e) { r.error=ConfigError::INVALID_JSON; r.diagnostics.push_back({"",e.what(),false}); return r; } if(!tracker.duplicate().empty()) { r.error=ConfigError::INVALID_JSON; r.diagnostics.push_back({"","duplicate key: "+tracker.duplicate(),false}); return r; } if(!validate(parsed,r.diagnostics)) { r.error=ConfigError::INVALID_SCHEMA; return r; } { std::unique_lock lock(impl_->mutex); impl_->snapshot=std::make_shared<json>(std::move(parsed)); impl_->file_path=file; impl_->diagnostics=r.diagnostics; ++impl_->version; } r.success=true; r.error=ConfigError::OK; return r; }
ConfigLoadResult ConfigStore::reload() { std::filesystem::path p; { std::shared_lock lock(impl_->mutex); p=impl_->file_path; } if(p.empty()) return {false,ConfigError::NOT_LOADED,{{"","no configuration path",false}}}; return load(p); }
std::filesystem::path ConfigStore::path() const { std::shared_lock lock(impl_->mutex); return impl_->file_path; }
std::vector<ConfigDiagnostic> ConfigStore::diagnostics() const { std::shared_lock lock(impl_->mutex); return impl_->diagnostics; }
void ConfigStore::setChangeCallback(ChangeCallback callback) { std::unique_lock lock(impl_->mutex); impl_->callback=std::move(callback); }
PersistResult ConfigStore::updateRuntime(const char* key,const ConfigValue& value,UpdateSource source,UpdateMode mode) {
    if(!key||!*key) return {false,false,false,ConfigError::MISSING,"empty key"}; if(!allowed(source,mode)) return {false,false,false,ConfigError::NOT_AUTHORIZED,"only WEB in MANUAL or DEBUG may update"};
    ChangeCallback callback; std::uint64_t version=0; PersistResult result{false,false,false,ConfigError::NOT_LOADED,"configuration is not loaded"};
    { std::unique_lock lock(impl_->mutex); if(!impl_->snapshot) return result; const json* original=findPath(*impl_->snapshot,key); if(!original) return {false,false,false,ConfigError::MISSING,"unknown configuration key"}; if(!compatible(*original,value)) return {false,false,false,ConfigError::INVALID_TYPE,"value type does not match"}; if(const auto numeric=std::get_if<double>(&value); numeric&&!std::isfinite(*numeric)) return {false,false,false,ConfigError::OUT_OF_RANGE,"value must be finite"}; json candidate=*impl_->snapshot; json* current=mutablePath(candidate,key); if(!current) return {false,false,false,ConfigError::MISSING,"unknown configuration key"}; *current=toJson(value); std::vector<ConfigDiagnostic> validation; if(!validate(candidate,validation)) return {false,false,false,ConfigError::OUT_OF_RANGE,validation.empty()?"updated value violates configuration schema":validation.front().key+": "+validation.front().message}; const std::string text=candidate.dump(); impl_->snapshot=std::make_shared<json>(std::move(candidate)); ++impl_->version; version=impl_->version; callback=impl_->callback; result={true,true,false,ConfigError::PERSISTENCE_FAILED,"effective value updated but persistence failed"}; std::ofstream output(impl_->file_path,std::ios::binary|std::ios::trunc); if(output) { output<<text; result.persisted=static_cast<bool>(output); if(result.persisted) { result.error=ConfigError::OK; result.message="configuration updated"; } } }
    if(callback) callback(key,version); return result;
}
PidReadResult ConfigStore::readPid(std::int64_t motor_id) const {
    PidReadResult r; std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) { r.message="configuration is not loaded"; return r; } const auto motors=member(impl_->snapshot.get(),"motor"); if(!motors||!motors->is_array()) { r.error=ConfigError::INVALID_SCHEMA; r.message="motor is not an array"; return r; }
    for(const auto& motor:*motors) { const auto id=member(&motor,"id"); if(id&&integer(*id)&&intValue(*id)==motor_id) { const auto pid=member(&motor,"pid"); if(!pid||!pid->is_object()) { r.error=ConfigError::MISSING; r.message="motor PID is missing"; return r; } auto read=[&](const char* k,std::int64_t& x){const auto v=member(pid,k); if(!v||!integer(*v)) return false; x=intValue(*v); return true;}; if(!read("kp",r.value.kp)||!read("ki",r.value.ki)||!read("kd",r.value.kd)||!read("integral_limit",r.value.integral_limit)||!read("output_limit",r.value.output_limit)||!read("control_period_ms",r.value.control_period_ms)) { r.error=ConfigError::INVALID_TYPE; r.message="invalid PID integer"; return r; } const auto enabled=member(pid,"enabled"); if(!enabled||!enabled->is_boolean()) { r.error=ConfigError::INVALID_TYPE; r.message="invalid PID enabled flag"; return r; } r.value.enabled=enabled->get<bool>(); r.success=true; r.error=ConfigError::OK; return r; } }
    r.error=ConfigError::NOT_FOUND; r.message="motor id not found"; return r;
}
PersistResult ConfigStore::writePid(std::int64_t motor_id,const PidConfig& value,UpdateSource source,UpdateMode mode) {
    if(!allowed(source,mode)) return {false,false,false,ConfigError::NOT_AUTHORIZED,"only WEB in MANUAL or DEBUG may update PID"}; if(value.integral_limit<0||value.output_limit<0||value.control_period_ms<=0) return {false,false,false,ConfigError::OUT_OF_RANGE,"invalid PID limits or period"};
    ChangeCallback callback; std::uint64_t version=0; PersistResult result{false,false,false,ConfigError::NOT_LOADED,"configuration is not loaded"};
    { std::unique_lock lock(impl_->mutex); if(!impl_->snapshot) return result; json candidate=*impl_->snapshot; auto motors=member(&candidate,"motor"); json* target=nullptr; if(motors&&motors->is_array()) for(auto& motor:*motors) { auto id=member(&motor,"id"); if(id&&integer(*id)&&intValue(*id)==motor_id) target=&motor; } if(!target) return {false,false,false,ConfigError::NOT_FOUND,"motor id not found"}; auto pid=member(target,"pid"); if(!pid||!pid->is_object()) return {false,false,false,ConfigError::MISSING,"motor PID is missing"}; const std::pair<const char*,json> fields[]={{"kp",json(value.kp)},{"ki",json(value.ki)},{"kd",json(value.kd)},{"integral_limit",json(value.integral_limit)},{"output_limit",json(value.output_limit)},{"control_period_ms",json(value.control_period_ms)},{"enabled",json(value.enabled)}}; for(const auto& [key,v]:fields) { auto field=member(pid,key); if(!field) return {false,false,false,ConfigError::MISSING,std::string("missing PID field: ")+key}; *field=v; } std::vector<ConfigDiagnostic> validation; if(!validate(candidate,validation)) return {false,false,false,ConfigError::OUT_OF_RANGE,validation.empty()?"PID violates configuration schema":validation.front().key+": "+validation.front().message}; const std::string text=candidate.dump(); impl_->snapshot=std::make_shared<json>(std::move(candidate)); ++impl_->version; version=impl_->version; callback=impl_->callback; result={true,true,false,ConfigError::PERSISTENCE_FAILED,"effective PID updated but persistence failed"}; std::ofstream output(impl_->file_path,std::ios::binary|std::ios::trunc); if(output) { output<<text; result.persisted=static_cast<bool>(output); if(result.persisted) { result.error=ConfigError::OK; result.message="PID updated"; } } }
    if(callback) callback("motor.pid",version); return result;
}
PersistResult ConfigStore::readReserved(std::uint32_t index,std::int32_t& value) const { if(index==0||index>100) return {false,false,false,ConfigError::OUT_OF_RANGE,"reserved index must be 1..100"}; std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return {false,false,false,ConfigError::NOT_LOADED,"configuration is not loaded"}; const auto v=member(impl_->snapshot.get(),reservedKey(index)); if(!v) return {false,false,false,ConfigError::NOT_FOUND,"reserved variable not found"}; if(!integer(*v)||intValue(*v)<INT32_MIN||intValue(*v)>INT32_MAX) return {false,false,false,ConfigError::INVALID_TYPE,"reserved variable is not int32"}; value=static_cast<std::int32_t>(intValue(*v)); return {true,true,true,ConfigError::OK,"reserved variable read"}; }
PersistResult ConfigStore::writeReserved(std::uint32_t index,std::int32_t value,UpdateSource source,UpdateMode mode) {
    if(index==0||index>100) return {false,false,false,ConfigError::OUT_OF_RANGE,"reserved index must be 1..100"};
    if(!allowed(source,mode)) return {false,false,false,ConfigError::NOT_AUTHORIZED,"only WEB in MANUAL or DEBUG may update"};
    const auto key=reservedKey(index); ChangeCallback callback; std::uint64_t version=0; PersistResult result{false,false,false,ConfigError::NOT_LOADED,"configuration is not loaded"};
    { std::unique_lock lock(impl_->mutex); if(!impl_->snapshot) return result; json candidate=*impl_->snapshot; candidate[key]=value; std::vector<ConfigDiagnostic> validation; if(!validate(candidate,validation)) return {false,false,false,ConfigError::OUT_OF_RANGE,validation.empty()?"reserved variable violates configuration schema":validation.front().key+": "+validation.front().message}; const std::string text=candidate.dump(); impl_->snapshot=std::make_shared<json>(std::move(candidate)); ++impl_->version; version=impl_->version; callback=impl_->callback; result={true,true,false,ConfigError::PERSISTENCE_FAILED,"effective reserved variable updated but persistence failed"}; std::ofstream output(impl_->file_path,std::ios::binary|std::ios::trunc); if(output) { output<<text; result.persisted=static_cast<bool>(output); if(result.persisted) { result.error=ConfigError::OK; result.message="reserved variable updated"; } } }
    if(callback) callback(key,version); return result;
}
config::ConfigStatus ConfigStore::getInt(const char* key,std::int64_t& out) const noexcept { try { std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return config::ConfigStatus::NOT_LOADED; const auto v=findPath(*impl_->snapshot,key?key:""); if(!v) return config::ConfigStatus::MISSING; if(!integer(*v)) return config::ConfigStatus::INVALID_TYPE; out=intValue(*v); return config::ConfigStatus::OK; } catch(...) { return config::ConfigStatus::INVALID_TYPE; } }
config::ConfigStatus ConfigStore::getDouble(const char* key,double& out) const noexcept { try { std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return config::ConfigStatus::NOT_LOADED; const auto v=findPath(*impl_->snapshot,key?key:""); if(!v) return config::ConfigStatus::MISSING; if(!number(*v)) return config::ConfigStatus::INVALID_TYPE; out=v->get<double>(); return config::ConfigStatus::OK; } catch(...) { return config::ConfigStatus::INVALID_TYPE; } }
config::ConfigStatus ConfigStore::getBool(const char* key,bool& out) const noexcept { try { std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return config::ConfigStatus::NOT_LOADED; const auto v=findPath(*impl_->snapshot,key?key:""); if(!v) return config::ConfigStatus::MISSING; if(!v->is_boolean()) return config::ConfigStatus::INVALID_TYPE; out=v->get<bool>(); return config::ConfigStatus::OK; } catch(...) { return config::ConfigStatus::INVALID_TYPE; } }
config::ConfigStatus ConfigStore::getString(const char* key,const char*& data,std::uint64_t& size) const noexcept { try { std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return config::ConfigStatus::NOT_LOADED; const auto v=findPath(*impl_->snapshot,key?key:""); if(!v) return config::ConfigStatus::MISSING; if(!v->is_string()) return config::ConfigStatus::INVALID_TYPE; const auto& s=v->get_ref<const std::string&>(); data=s.data(); size=s.size(); return config::ConfigStatus::OK; } catch(...) { return config::ConfigStatus::INVALID_TYPE; } }
bool ConfigStore::has(const char* key) const noexcept { try { std::shared_lock lock(impl_->mutex); return impl_->snapshot&&findPath(*impl_->snapshot,key?key:""); } catch(...) { return false; } }
config::ConfigStatus ConfigStore::getArraySize(const char* key,std::uint64_t& size) const noexcept { try { std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return config::ConfigStatus::NOT_LOADED; const auto v=findPath(*impl_->snapshot,key?key:""); if(!v) return config::ConfigStatus::MISSING; if(!v->is_array()) return config::ConfigStatus::INVALID_TYPE; size=v->size(); return config::ConfigStatus::OK; } catch(...) { return config::ConfigStatus::INVALID_TYPE; } }

template<class T> config::ConfigStatus arrayField(const json& root,const char* key,std::uint64_t index,const char* field,T& out) noexcept { const auto a=findPath(root,key?key:""); if(!a) return config::ConfigStatus::MISSING; if(!a->is_array()||index>=a->size()) return config::ConfigStatus::OUT_OF_RANGE; const auto& element=(*a)[static_cast<std::size_t>(index)]; const auto v=(field&&*field)?member(&element,field):&element; if(!v) return config::ConfigStatus::MISSING; if constexpr(std::is_same_v<T,std::int64_t>) { if(!integer(*v)) return config::ConfigStatus::INVALID_TYPE; out=intValue(*v); } else if constexpr(std::is_same_v<T,double>) { if(!number(*v)) return config::ConfigStatus::INVALID_TYPE; out=v->get<double>(); } else { if(!v->is_boolean()) return config::ConfigStatus::INVALID_TYPE; out=v->get<bool>(); } return config::ConfigStatus::OK; }
config::ConfigStatus ConfigStore::getIntAt(const char* key,std::uint64_t index,const char* field,std::int64_t& out) const noexcept { try { std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return config::ConfigStatus::NOT_LOADED; return arrayField(*impl_->snapshot,key,index,field,out); } catch(...) { return config::ConfigStatus::INVALID_TYPE; } }
config::ConfigStatus ConfigStore::getDoubleAt(const char* key,std::uint64_t index,const char* field,double& out) const noexcept { try { std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return config::ConfigStatus::NOT_LOADED; return arrayField(*impl_->snapshot,key,index,field,out); } catch(...) { return config::ConfigStatus::INVALID_TYPE; } }
config::ConfigStatus ConfigStore::getBoolAt(const char* key,std::uint64_t index,const char* field,bool& out) const noexcept { try { std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return config::ConfigStatus::NOT_LOADED; return arrayField(*impl_->snapshot,key,index,field,out); } catch(...) { return config::ConfigStatus::INVALID_TYPE; } }
config::ConfigStatus ConfigStore::getStringAt(const char* key,std::uint64_t index,const char* field,const char*& data,std::uint64_t& size) const noexcept { try { std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return config::ConfigStatus::NOT_LOADED; const auto a=findPath(*impl_->snapshot,key?key:""); if(!a) return config::ConfigStatus::MISSING; if(!a->is_array()||index>=a->size()) return config::ConfigStatus::OUT_OF_RANGE; const auto& element=(*a)[static_cast<std::size_t>(index)]; const auto v=(field&&*field)?member(&element,field):&element; if(!v) return config::ConfigStatus::MISSING; if(!v->is_string()) return config::ConfigStatus::INVALID_TYPE; const auto& s=v->get_ref<const std::string&>(); data=s.data(); size=s.size(); return config::ConfigStatus::OK; } catch(...) { return config::ConfigStatus::INVALID_TYPE; } }
std::uint64_t ConfigStore::configVersion() const noexcept { try { std::shared_lock lock(impl_->mutex); return impl_->version; } catch(...) { return 0; } }
bool ConfigStore::isLoaded() const noexcept { try { std::shared_lock lock(impl_->mutex); return static_cast<bool>(impl_->snapshot); } catch(...) { return false; } }
runtime::ModuleInitResult LifecycleAdapter::initialize(const runtime::RuntimeContext& context) { const auto result=store_.load(context.configuration_path); runtime::ModuleInitResult output; output.module_name=name(); output.success=result.success; if(!result.success) output.error_message=result.diagnostics.empty()?errorName(result.error):result.diagnostics.front().key+": "+result.diagnostics.front().message; return output; }
void LifecycleAdapter::shutdown() noexcept {}
}
