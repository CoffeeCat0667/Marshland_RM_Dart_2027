#include "atom15_config/config_store.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <utility>
namespace marshland::atom15 {
namespace {
struct JsonValue final {
    enum class Kind : std::uint8_t { Null, Boolean, Integer, Number, String, Array, Object };
    Kind kind{Kind::Null}; bool boolean_value{false}; std::int64_t integer_value{0}; double number_value{0};
    std::string string_value; std::vector<JsonValue> array_value; std::map<std::string, JsonValue, std::less<>> object_value;
    static JsonValue null() { return {}; }
    static JsonValue boolean(bool v) { JsonValue x; x.kind=Kind::Boolean; x.boolean_value=v; return x; }
    static JsonValue integer(std::int64_t v) { JsonValue x; x.kind=Kind::Integer; x.integer_value=v; return x; }
    static JsonValue number(double v) { JsonValue x; x.kind=Kind::Number; x.number_value=v; return x; }
    static JsonValue string(std::string v) { JsonValue x; x.kind=Kind::String; x.string_value=std::move(v); return x; }
    static JsonValue array() { JsonValue x; x.kind=Kind::Array; return x; }
    static JsonValue object() { JsonValue x; x.kind=Kind::Object; return x; }
};
class Parser final {
public:
    explicit Parser(std::string_view text): text_(text) {}
    bool parse(JsonValue& out, std::string& error) { skip(); if (!value(out,error)) return false; skip(); if (pos_ != text_.size()) { error="trailing data"; return false; } return true; }
private:
    void skip() noexcept { while(pos_<text_.size() && (text_[pos_]==' '||text_[pos_]=='\n'||text_[pos_]=='\r'||text_[pos_]=='\t')) ++pos_; }
    bool eat(char c) noexcept { if(pos_<text_.size() && text_[pos_]==c) { ++pos_; return true; } return false; }
    bool literal(std::string_view s, JsonValue v, JsonValue& out, std::string& e) { if(text_.substr(pos_,s.size())!=s) { e="invalid literal"; return false; } pos_+=s.size(); out=std::move(v); return true; }
    bool value(JsonValue& out, std::string& e) {
        skip(); if(pos_>=text_.size()) { e="unexpected end"; return false; }
        switch(text_[pos_]) { case '{': return object(out,e); case '[': return array(out,e); case '"': { std::string s; if(!string(s,e)) return false; out=JsonValue::string(std::move(s)); return true; } case 't': return literal("true",JsonValue::boolean(true),out,e); case 'f': return literal("false",JsonValue::boolean(false),out,e); case 'n': return literal("null",JsonValue::null(),out,e); default: return number(out,e); }
    }
    bool object(JsonValue& out, std::string& e) {
        out=JsonValue::object(); ++pos_; skip(); if(eat('}')) return true;
        while(pos_<text_.size()) { std::string key; if(!string(key,e)) return false; skip(); if(!eat(':')) { e="expected ':'"; return false; } JsonValue v; if(!value(v,e)) return false; if(!out.object_value.emplace(key,std::move(v)).second) { e="duplicate key: "+key; return false; } skip(); if(eat('}')) return true; if(!eat(',')) { e="expected ',' or '}'"; return false; } skip(); }
        e="unterminated object"; return false;
    }
    bool array(JsonValue& out, std::string& e) {
        out=JsonValue::array(); ++pos_; skip(); if(eat(']')) return true;
        while(pos_<text_.size()) { JsonValue v; if(!value(v,e)) return false; out.array_value.push_back(std::move(v)); skip(); if(eat(']')) return true; if(!eat(',')) { e="expected ',' or ']'"; return false; } skip(); }
        e="unterminated array"; return false;
    }
    bool string(std::string& out, std::string& e) {
        if(!eat('"')) { e="expected string"; return false; } out.clear();
        const auto hex = [](char c) noexcept -> int {
            if(c>='0'&&c<='9') return c-'0';
            if(c>='a'&&c<='f') return c-'a'+10;
            if(c>='A'&&c<='F') return c-'A'+10;
            return -1;
        };
        const auto appendUtf8 = [&out](std::uint32_t cp) {
            if(cp<=0x7fU) out.push_back(static_cast<char>(cp));
            else if(cp<=0x7ffU) { out.push_back(static_cast<char>(0xc0U|(cp>>6U))); out.push_back(static_cast<char>(0x80U|(cp&0x3fU))); }
            else if(cp<=0xffffU) { out.push_back(static_cast<char>(0xe0U|(cp>>12U))); out.push_back(static_cast<char>(0x80U|((cp>>6U)&0x3fU))); out.push_back(static_cast<char>(0x80U|(cp&0x3fU))); }
            else { out.push_back(static_cast<char>(0xf0U|(cp>>18U))); out.push_back(static_cast<char>(0x80U|((cp>>12U)&0x3fU))); out.push_back(static_cast<char>(0x80U|((cp>>6U)&0x3fU))); out.push_back(static_cast<char>(0x80U|(cp&0x3fU))); }
        };
        while(pos_<text_.size()) {
            const char c=text_[pos_++];
            if(c=='"') return true;
            if(c=='\\') {
                if(pos_>=text_.size()) { e="unterminated escape"; return false; }
                const char q=text_[pos_++];
                switch(q) {
                case '"': out+='"'; break; case '\\': out+='\\'; break; case '/': out+='/'; break;
                case 'b': out+='\b'; break; case 'f': out+='\f'; break; case 'n': out+='\n'; break;
                case 'r': out+='\r'; break; case 't': out+='\t'; break;
                case 'u': {
                    if(pos_+4>text_.size()) { e="incomplete unicode escape"; return false; }
                    std::uint32_t cp=0;
                    for(int i=0;i<4;++i) { const int digit=hex(text_[pos_++]); if(digit<0) { e="invalid unicode escape"; return false; } cp=(cp<<4U)|static_cast<std::uint32_t>(digit); }
                    if(cp>=0xd800U&&cp<=0xdbffU) {
                        if(pos_+6>text_.size()||text_[pos_++]!='\\'||text_[pos_++]!='u') { e="missing unicode surrogate pair"; return false; }
                        std::uint32_t low=0; for(int i=0;i<4;++i) { const int digit=hex(text_[pos_++]); if(digit<0) { e="invalid unicode surrogate"; return false; } low=(low<<4U)|static_cast<std::uint32_t>(digit); }
                        if(low<0xdc00U||low>0xdfffU) { e="invalid unicode surrogate pair"; return false; }
                        cp=0x10000U+((cp-0xd800U)<<10U)+(low-0xdc00U);
                    } else if(cp>=0xdc00U&&cp<=0xdfffU) { e="unexpected low surrogate"; return false; }
                    appendUtf8(cp); break;
                }
                default: e="unsupported escape"; return false;
                }
            } else if(static_cast<unsigned char>(c)<0x20U) { e="control character"; return false; }
            else out+=c;
        }
        e="unterminated string"; return false;
    }
    bool number(JsonValue& out, std::string& e) {
        const auto begin=pos_; if(pos_<text_.size()&&text_[pos_]=='-') ++pos_; if(pos_>=text_.size()||text_[pos_]<'0'||text_[pos_]>'9') { e="invalid number"; return false; }
        if(text_[pos_]=='0') ++pos_; else while(pos_<text_.size()&&text_[pos_]>='0'&&text_[pos_]<='9') ++pos_; bool real=false;
        if(pos_<text_.size()&&text_[pos_]=='.') { real=true; ++pos_; const auto fraction=pos_; while(pos_<text_.size()&&text_[pos_]>='0'&&text_[pos_]<='9') ++pos_; if(pos_==fraction) { e="digits required after decimal point"; return false; } }
        if(pos_<text_.size()&&(text_[pos_]=='e'||text_[pos_]=='E')) { real=true; ++pos_; if(pos_<text_.size()&&(text_[pos_]=='+'||text_[pos_]=='-')) ++pos_; const auto exponent=pos_; while(pos_<text_.size()&&text_[pos_]>='0'&&text_[pos_]<='9') ++pos_; if(pos_==exponent) { e="digits required after exponent"; return false; } }
        try { const std::string s(text_.substr(begin,pos_-begin)); std::size_t consumed=0; if(real) { const double value=std::stod(s,&consumed); if(consumed!=s.size()||!std::isfinite(value)) { e="invalid or non-finite number"; return false; } out=JsonValue::number(value); } else { const auto value=std::stoll(s,&consumed); if(consumed!=s.size()) { e="invalid integer"; return false; } out=JsonValue::integer(value); } return true; } catch(...) { e="number out of range"; return false; }
    }
    std::string_view text_; std::size_t pos_{0};
};
const JsonValue* member(const JsonValue* v, std::string_view key) noexcept { if(!v||v->kind!=JsonValue::Kind::Object) return nullptr; auto i=v->object_value.find(key); return i==v->object_value.end()?nullptr:&i->second; }
JsonValue* member(JsonValue* v, std::string_view key) noexcept { if(!v||v->kind!=JsonValue::Kind::Object) return nullptr; auto i=v->object_value.find(key); return i==v->object_value.end()?nullptr:&i->second; }
const JsonValue* findPath(const JsonValue& root, std::string_view key) noexcept { const JsonValue* v=&root; std::size_t b=0; while(b<key.size()) { const auto e=key.find('.',b); v=member(v,key.substr(b,e==std::string_view::npos?key.size()-b:e-b)); if(!v) return nullptr; if(e==std::string_view::npos) break; b=e+1; } return v; }
JsonValue* mutablePath(JsonValue& root, std::string_view key) { JsonValue* v=&root; std::size_t b=0; while(b<key.size()) { const auto e=key.find('.',b); v=member(v,key.substr(b,e==std::string_view::npos?key.size()-b:e-b)); if(!v) return nullptr; if(e==std::string_view::npos) break; b=e+1; } return v; }
bool integer(const JsonValue& v) noexcept { return v.kind==JsonValue::Kind::Integer; }
bool number(const JsonValue& v) noexcept { return v.kind==JsonValue::Kind::Integer||v.kind==JsonValue::Kind::Number; }
void diagnostic(std::vector<ConfigDiagnostic>& d, std::string key, std::string message) { d.push_back({std::move(key),std::move(message),false}); }
bool requiredObject(const JsonValue* v, const char* key, std::vector<ConfigDiagnostic>& d) { if(!v) { diagnostic(d,key,"missing required object"); return false; } if(v->kind!=JsonValue::Kind::Object) { diagnostic(d,key,"expected object"); return false; } return true; }
bool requiredMember(const JsonValue* object, const char* key, JsonValue::Kind kind, std::vector<ConfigDiagnostic>& d) { const auto v=member(object,key); if(!v) { diagnostic(d,key,"missing required value"); return false; } if(v->kind==JsonValue::Kind::Null) { diagnostic(d,key,"value is not implemented"); return false; } if(kind==JsonValue::Kind::Integer ? !integer(*v) : v->kind!=kind) { diagnostic(d,key,"invalid type"); return false; } return true; }bool integerRange(const JsonValue* object,const char* key,std::int64_t minimum,std::int64_t maximum,std::vector<ConfigDiagnostic>& d) { const auto v=member(object,key); if(!v) { diagnostic(d,key,"missing required value"); return false; } if(!integer(*v)) { diagnostic(d,key,"invalid type"); return false; } if(v->integer_value<minimum||v->integer_value>maximum) { diagnostic(d,key,"out of range"); return false; } return true; }
bool nonEmptyString(const JsonValue* object,const char* key,std::vector<ConfigDiagnostic>& d) { const auto v=member(object,key); if(!v) { diagnostic(d,key,"missing required value"); return false; } if(v->kind!=JsonValue::Kind::String||v->string_value.empty()) { diagnostic(d,key,"must be a non-empty string"); return false; } return true; }
bool isReservedKey(std::string_view key) noexcept {
    if(key.size()!=12||key.substr(0,9)!="reserved_") return false;
    return key[9]>='0'&&key[9]<='9'&&key[10]>='0'&&key[10]<='9'&&key[11]>='0'&&key[11]<='9';
}
bool isKnownRootKey(std::string_view key) noexcept {
    if(isReservedKey(key)) return true;
    for(const auto known: {"schema_version","baseline","mode","timing","motor","can","sbus","persistence","gpio","servo","vision","http","websocket","fault","thread","log"}) if(key==known) return true;
    return false;
}
bool validate(const JsonValue& root, std::vector<ConfigDiagnostic>& d) {
    if(root.kind!=JsonValue::Kind::Object) { diagnostic(d,"","root must be an object"); return false; }
    for(const auto& [key,value]:root.object_value) if(!isKnownRootKey(key)) diagnostic(d,key,"unknown top-level property");
    const auto schema=member(&root,"schema_version"); if(!schema||!integer(*schema)||schema->integer_value!=1) diagnostic(d,"schema_version","must be integer 1");
    for(const char* key:{"baseline","mode","timing","motor","can","sbus","persistence"}) if(!member(&root,key)) diagnostic(d,key,"missing required top-level section");
    requiredObject(member(&root,"baseline"),"baseline",d); requiredObject(member(&root,"mode"),"mode",d); requiredObject(member(&root,"timing"),"timing",d); requiredObject(member(&root,"can"),"can",d); requiredObject(member(&root,"sbus"),"sbus",d); requiredObject(member(&root,"persistence"),"persistence",d);
    for(const char* key:{"gpio","servo","vision","http","websocket","fault","thread","log"}) if(const auto section=member(&root,key); section&&section->kind!=JsonValue::Kind::Object) diagnostic(d,key,"must be an object");
    const auto motors=member(&root,"motor");
    if(!motors||motors->kind!=JsonValue::Kind::Array||motors->array_value.empty()) diagnostic(d,"motor","must be a non-empty array");
    std::vector<std::int64_t> motorIds; std::vector<std::int64_t> motorNodes;
    if(motors&&motors->kind==JsonValue::Kind::Array) for(std::size_t i=0;i<motors->array_value.size();++i) {
        const auto& m=motors->array_value[i]; const auto p="motor["+std::to_string(i)+"]";
        if(m.kind!=JsonValue::Kind::Object) { diagnostic(d,p,"must be an object"); continue; }
        for(const char* key:{"id","role","bus","node","target_unit","pid"}) if(!member(&m,key)) diagnostic(d,p+"."+key,"missing required motor field");
        const auto id=member(&m,"id"); if(id&&integer(*id)) { if(std::find(motorIds.begin(),motorIds.end(),id->integer_value)!=motorIds.end()) diagnostic(d,p+".id","duplicate motor id"); motorIds.push_back(id->integer_value); }
        const auto node=member(&m,"node"); if(node&&integer(*node)) { if(std::find(motorNodes.begin(),motorNodes.end(),node->integer_value)!=motorNodes.end()) diagnostic(d,p+".node","duplicate motor node"); motorNodes.push_back(node->integer_value); }
        const auto pid=member(&m,"pid"); if(pid&&pid->kind==JsonValue::Kind::Object) { for(const char* key:{"kp","ki","kd","integral_limit","output_limit","control_period_ms"}) requiredMember(pid,key,JsonValue::Kind::Integer,d); requiredMember(pid,"enabled",JsonValue::Kind::Boolean,d); const auto period=member(pid,"control_period_ms"); if(period&&integer(*period)&&period->integer_value<=0) diagnostic(d,p+".pid.control_period_ms","must be positive"); }
    }
    const auto sbus=member(&root,"sbus"); if(sbus&&sbus->kind==JsonValue::Kind::Object) { nonEmptyString(sbus,"serial_device",d); integerRange(sbus,"channel_scale_x",1,10,d); integerRange(sbus,"frame_bytes",25,25,d); integerRange(sbus,"joystick_center",0,2047,d); integerRange(sbus,"valid_min",0,2047,d); integerRange(sbus,"valid_max",0,2047,d); const auto min=member(sbus,"valid_min"); const auto max=member(sbus,"valid_max"); if(min&&max&&integer(*min)&&integer(*max)&&min->integer_value>max->integer_value) diagnostic(d,"sbus.valid_min","must not exceed valid_max"); }
    const auto timing=member(&root,"timing"); if(timing&&timing->kind==JsonValue::Kind::Object) { integerRange(timing,"sbus_receive_period_ms",1,86400000,d); integerRange(timing,"sbus_loss_timeout_ms",1,86400000,d); }
    const auto persistence=member(&root,"persistence"); if(persistence&&persistence->kind==JsonValue::Kind::Object) { for(const char* key:{"file_format","directory","file_name"}) if(!member(persistence,key)) diagnostic(d,std::string("persistence.")+key,"missing required value"); const auto count=member(persistence,"reserved_variable_count"); if(count&&(!integer(*count)||count->integer_value!=100)) diagnostic(d,"persistence.reserved_variable_count","must be integer 100"); }
    for(const auto& [key,value]:root.object_value) if(isReservedKey(key)&&(!integer(value)||key<"reserved_001"||key>"reserved_100")) diagnostic(d,key,"reserved variable must be an int32-compatible reserved_001..reserved_100 value");
    return d.empty();
}
void serialize(const JsonValue& v, std::string& out) {
    switch(v.kind) { case JsonValue::Kind::Null: out+="null"; break; case JsonValue::Kind::Boolean: out+=v.boolean_value?"true":"false"; break; case JsonValue::Kind::Integer: out+=std::to_string(v.integer_value); break; case JsonValue::Kind::Number: { std::ostringstream s; s.precision(17); s<<v.number_value; out+=s.str(); break; } case JsonValue::Kind::String: out.push_back('"'); for(const unsigned char c: v.string_value) { switch(c) { case '"': out+="\\\""; break; case '\\': out+="\\\\"; break; case '\b': out+="\\b"; break; case '\f': out+="\\f"; break; case '\n': out+="\\n"; break; case '\r': out+="\\r"; break; case '\t': out+="\\t"; break; default: if(c<0x20U) { char escaped[7]{}; std::snprintf(escaped,sizeof(escaped),"\\u%04x",c); out+=escaped; } else out.push_back(static_cast<char>(c)); } } out.push_back('"'); break; case JsonValue::Kind::Array: out.push_back('['); for(std::size_t i=0;i<v.array_value.size();++i) { if(i) out.push_back(','); serialize(v.array_value[i],out); } out.push_back(']'); break; case JsonValue::Kind::Object: out.push_back('{'); { bool first=true; for(const auto& [k,x]:v.object_value) { if(!first) out.push_back(','); first=false; serialize(JsonValue::string(k),out); out.push_back(':'); serialize(x,out); } } out.push_back('}'); break; }
}
JsonValue toJson(const ConfigValue& value) { return std::visit([](const auto& x)->JsonValue { using T=std::decay_t<decltype(x)>; if constexpr(std::is_same_v<T,std::int64_t>) return JsonValue::integer(x); else if constexpr(std::is_same_v<T,double>) return JsonValue::number(x); else if constexpr(std::is_same_v<T,bool>) return JsonValue::boolean(x); else return JsonValue::string(x); },value); }
bool compatible(const JsonValue& current,const ConfigValue& value) noexcept { return std::visit([&](const auto& x) { using T=std::decay_t<decltype(x)>; if constexpr(std::is_same_v<T,std::int64_t>) return current.kind==JsonValue::Kind::Integer; else if constexpr(std::is_same_v<T,double>) return number(current); else if constexpr(std::is_same_v<T,bool>) return current.kind==JsonValue::Kind::Boolean; else return current.kind==JsonValue::Kind::String; },value); }
bool allowed(UpdateSource source,UpdateMode mode) noexcept { return source==UpdateSource::WEB&&(mode==UpdateMode::MANUAL||mode==UpdateMode::DEBUG); }
const char* errorName(ConfigError e) noexcept { switch(e) { case ConfigError::OK:return "OK"; case ConfigError::FILE_NOT_FOUND:return "FILE_NOT_FOUND"; case ConfigError::FILE_IO:return "FILE_IO"; case ConfigError::INVALID_JSON:return "INVALID_JSON"; case ConfigError::INVALID_SCHEMA:return "INVALID_SCHEMA"; case ConfigError::NOT_LOADED:return "NOT_LOADED"; case ConfigError::MISSING:return "MISSING"; case ConfigError::INVALID_TYPE:return "INVALID_TYPE"; case ConfigError::OUT_OF_RANGE:return "OUT_OF_RANGE"; case ConfigError::NOT_AUTHORIZED:return "NOT_AUTHORIZED"; case ConfigError::PERSISTENCE_FAILED:return "PERSISTENCE_FAILED"; case ConfigError::LIMIT_REACHED:return "LIMIT_REACHED"; case ConfigError::NOT_FOUND:return "NOT_FOUND"; } return "UNKNOWN"; }
std::string reservedKey(std::uint32_t index) { return std::string("reserved_")+(index<10?"00":index<100?"0":"")+std::to_string(index); }
} // namespace
struct ConfigStore::Impl final { mutable std::shared_mutex mutex; std::shared_ptr<JsonValue> snapshot; std::filesystem::path file_path; std::vector<ConfigDiagnostic> diagnostics; ChangeCallback callback; std::uint64_t version{0}; };
ConfigStore::ConfigStore(ChangeCallback callback):impl_(std::make_unique<Impl>()) { impl_->callback=std::move(callback); }
ConfigStore::~ConfigStore()=default;
ConfigLoadResult ConfigStore::load(const std::filesystem::path& file) { ConfigLoadResult r; std::ifstream input(file,std::ios::binary); if(!input) { r.error=std::filesystem::exists(file)?ConfigError::FILE_IO:ConfigError::FILE_NOT_FOUND; r.diagnostics.push_back({file.string(),errorName(r.error),false}); return r; } std::ostringstream b; b<<input.rdbuf(); JsonValue parsed; std::string error; if(!Parser(b.str()).parse(parsed,error)) { r.error=ConfigError::INVALID_JSON; r.diagnostics.push_back({"",error,false}); return r; } if(!validate(parsed,r.diagnostics)) { r.error=ConfigError::INVALID_SCHEMA; return r; } { std::unique_lock lock(impl_->mutex); impl_->snapshot=std::make_shared<JsonValue>(std::move(parsed)); impl_->file_path=file; impl_->diagnostics=r.diagnostics; ++impl_->version; } r.success=true; r.error=ConfigError::OK; return r; }
ConfigLoadResult ConfigStore::reload() { std::filesystem::path p; { std::shared_lock lock(impl_->mutex); p=impl_->file_path; } if(p.empty()) return {false,ConfigError::NOT_LOADED,{{"","no configuration path",false}}}; return load(p); }
std::filesystem::path ConfigStore::path() const { std::shared_lock lock(impl_->mutex); return impl_->file_path; }
std::vector<ConfigDiagnostic> ConfigStore::diagnostics() const { std::shared_lock lock(impl_->mutex); return impl_->diagnostics; }
void ConfigStore::setChangeCallback(ChangeCallback callback) { std::unique_lock lock(impl_->mutex); impl_->callback=std::move(callback); }
PersistResult ConfigStore::updateRuntime(const char* key,const ConfigValue& value,UpdateSource source,UpdateMode mode) {
    if(!key||!*key) return {false,false,false,ConfigError::MISSING,"empty key"}; if(!allowed(source,mode)) return {false,false,false,ConfigError::NOT_AUTHORIZED,"only WEB in MANUAL or DEBUG may update"};
    ChangeCallback callback; std::uint64_t version=0; PersistResult result{false,false,false,ConfigError::NOT_LOADED,"configuration is not loaded"};
    { std::unique_lock lock(impl_->mutex); if(!impl_->snapshot) return result; const JsonValue* original=mutablePath(*impl_->snapshot,key); if(!original) return {false,false,false,ConfigError::MISSING,"unknown configuration key"}; if(!compatible(*original,value)) return {false,false,false,ConfigError::INVALID_TYPE,"value type does not match"}; if(const auto numeric=std::get_if<double>(&value); numeric&&!std::isfinite(*numeric)) return {false,false,false,ConfigError::OUT_OF_RANGE,"value must be finite"}; JsonValue candidate=*impl_->snapshot; JsonValue* current=mutablePath(candidate,key); if(!current) return {false,false,false,ConfigError::MISSING,"unknown configuration key"}; *current=toJson(value); std::vector<ConfigDiagnostic> validation; if(!validate(candidate,validation)) return {false,false,false,ConfigError::OUT_OF_RANGE,validation.empty()?"updated value violates configuration schema":validation.front().key+": "+validation.front().message}; std::string text; serialize(candidate,text); impl_->snapshot=std::make_shared<JsonValue>(std::move(candidate)); ++impl_->version; version=impl_->version; callback=impl_->callback; result={true,true,false,ConfigError::PERSISTENCE_FAILED,"effective value updated but persistence failed"}; std::ofstream output(impl_->file_path,std::ios::binary|std::ios::trunc); if(output) { output<<text; result.persisted=static_cast<bool>(output); if(result.persisted) { result.error=ConfigError::OK; result.message="configuration updated"; } } }
    if(callback) callback(key,version); return result;
}
PidReadResult ConfigStore::readPid(std::int64_t motor_id) const {
    PidReadResult r; std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) { r.message="configuration is not loaded"; return r; } const auto motors=member(impl_->snapshot.get(),"motor"); if(!motors||motors->kind!=JsonValue::Kind::Array) { r.error=ConfigError::INVALID_SCHEMA; r.message="motor is not an array"; return r; }
    for(const auto& motor:motors->array_value) { const auto id=member(&motor,"id"); if(id&&integer(*id)&&id->integer_value==motor_id) { const auto pid=member(&motor,"pid"); if(!pid||pid->kind!=JsonValue::Kind::Object) { r.error=ConfigError::MISSING; r.message="motor PID is missing"; return r; } auto read=[&](const char* k,std::int64_t& x){const auto v=member(pid,k); if(!v||!integer(*v)) return false; x=v->integer_value; return true;}; if(!read("kp",r.value.kp)||!read("ki",r.value.ki)||!read("kd",r.value.kd)||!read("integral_limit",r.value.integral_limit)||!read("output_limit",r.value.output_limit)||!read("control_period_ms",r.value.control_period_ms)) { r.error=ConfigError::INVALID_TYPE; r.message="invalid PID integer"; return r; } const auto enabled=member(pid,"enabled"); if(!enabled||enabled->kind!=JsonValue::Kind::Boolean) { r.error=ConfigError::INVALID_TYPE; r.message="invalid PID enabled flag"; return r; } r.value.enabled=enabled->boolean_value; r.success=true; r.error=ConfigError::OK; return r; } }
    r.error=ConfigError::NOT_FOUND; r.message="motor id not found"; return r;
}
PersistResult ConfigStore::writePid(std::int64_t motor_id,const PidConfig& value,UpdateSource source,UpdateMode mode) {
    if(!allowed(source,mode)) return {false,false,false,ConfigError::NOT_AUTHORIZED,"only WEB in MANUAL or DEBUG may update PID"}; if(value.integral_limit<0||value.output_limit<0||value.control_period_ms<=0) return {false,false,false,ConfigError::OUT_OF_RANGE,"invalid PID limits or period"};
    ChangeCallback callback; std::uint64_t version=0; PersistResult result{false,false,false,ConfigError::NOT_LOADED,"configuration is not loaded"};
    { std::unique_lock lock(impl_->mutex); if(!impl_->snapshot) return result; JsonValue candidate=*impl_->snapshot; auto motors=member(&candidate,"motor"); JsonValue* target=nullptr; if(motors&&motors->kind==JsonValue::Kind::Array) for(auto& motor:motors->array_value) { auto id=member(&motor,"id"); if(id&&integer(*id)&&id->integer_value==motor_id) target=&motor; } if(!target) return {false,false,false,ConfigError::NOT_FOUND,"motor id not found"}; auto pid=member(target,"pid"); if(!pid||pid->kind!=JsonValue::Kind::Object) return {false,false,false,ConfigError::MISSING,"motor PID is missing"}; const std::pair<const char*,JsonValue> fields[]={{"kp",JsonValue::integer(value.kp)},{"ki",JsonValue::integer(value.ki)},{"kd",JsonValue::integer(value.kd)},{"integral_limit",JsonValue::integer(value.integral_limit)},{"output_limit",JsonValue::integer(value.output_limit)},{"control_period_ms",JsonValue::integer(value.control_period_ms)},{"enabled",JsonValue::boolean(value.enabled)}}; for(const auto& [key,v]:fields) { auto field=member(pid,key); if(!field) return {false,false,false,ConfigError::MISSING,std::string("missing PID field: ")+key}; *field=v; } std::vector<ConfigDiagnostic> validation; if(!validate(candidate,validation)) return {false,false,false,ConfigError::OUT_OF_RANGE,validation.empty()?"PID violates configuration schema":validation.front().key+": "+validation.front().message}; std::string text; serialize(candidate,text); impl_->snapshot=std::make_shared<JsonValue>(std::move(candidate)); ++impl_->version; version=impl_->version; callback=impl_->callback; result={true,true,false,ConfigError::PERSISTENCE_FAILED,"effective PID updated but persistence failed"}; std::ofstream output(impl_->file_path,std::ios::binary|std::ios::trunc); if(output) { output<<text; result.persisted=static_cast<bool>(output); if(result.persisted) { result.error=ConfigError::OK; result.message="PID updated"; } } }
    if(callback) callback("motor.pid",version); return result;
}
PersistResult ConfigStore::readReserved(std::uint32_t index,std::int32_t& value) const { if(index==0||index>100) return {false,false,false,ConfigError::OUT_OF_RANGE,"reserved index must be 1..100"}; std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return {false,false,false,ConfigError::NOT_LOADED,"configuration is not loaded"}; const auto v=member(impl_->snapshot.get(),reservedKey(index)); if(!v) return {false,false,false,ConfigError::NOT_FOUND,"reserved variable not found"}; if(!integer(*v)||v->integer_value<INT32_MIN||v->integer_value>INT32_MAX) return {false,false,false,ConfigError::INVALID_TYPE,"reserved variable is not int32"}; value=static_cast<std::int32_t>(v->integer_value); return {true,true,true,ConfigError::OK,"reserved variable read"}; }
PersistResult ConfigStore::writeReserved(std::uint32_t index,std::int32_t value,UpdateSource source,UpdateMode mode) {
    if(index==0||index>100) return {false,false,false,ConfigError::OUT_OF_RANGE,"reserved index must be 1..100"};
    if(!allowed(source,mode)) return {false,false,false,ConfigError::NOT_AUTHORIZED,"only WEB in MANUAL or DEBUG may update"};
    const auto key=reservedKey(index); ChangeCallback callback; std::uint64_t version=0; PersistResult result{false,false,false,ConfigError::NOT_LOADED,"configuration is not loaded"};
    { std::unique_lock lock(impl_->mutex); if(!impl_->snapshot) return result; JsonValue candidate=*impl_->snapshot; candidate.object_value[key]=JsonValue::integer(value); std::vector<ConfigDiagnostic> validation; if(!validate(candidate,validation)) return {false,false,false,ConfigError::OUT_OF_RANGE,validation.empty()?"reserved variable violates configuration schema":validation.front().key+": "+validation.front().message}; std::string text; serialize(candidate,text); impl_->snapshot=std::make_shared<JsonValue>(std::move(candidate)); ++impl_->version; version=impl_->version; callback=impl_->callback; result={true,true,false,ConfigError::PERSISTENCE_FAILED,"effective reserved variable updated but persistence failed"}; std::ofstream output(impl_->file_path,std::ios::binary|std::ios::trunc); if(output) { output<<text; result.persisted=static_cast<bool>(output); if(result.persisted) { result.error=ConfigError::OK; result.message="reserved variable updated"; } } }
    if(callback) callback(key,version); return result;
}
config::ConfigStatus ConfigStore::getInt(const char* key,std::int64_t& out) const noexcept { try { std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return config::ConfigStatus::NOT_LOADED; const auto v=findPath(*impl_->snapshot,key?key:""); if(!v) return config::ConfigStatus::MISSING; if(!integer(*v)) return config::ConfigStatus::INVALID_TYPE; out=v->integer_value; return config::ConfigStatus::OK; } catch(...) { return config::ConfigStatus::INVALID_TYPE; } }
config::ConfigStatus ConfigStore::getDouble(const char* key,double& out) const noexcept { try { std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return config::ConfigStatus::NOT_LOADED; const auto v=findPath(*impl_->snapshot,key?key:""); if(!v) return config::ConfigStatus::MISSING; if(!number(*v)) return config::ConfigStatus::INVALID_TYPE; out=v->kind==JsonValue::Kind::Integer?static_cast<double>(v->integer_value):v->number_value; return config::ConfigStatus::OK; } catch(...) { return config::ConfigStatus::INVALID_TYPE; } }
config::ConfigStatus ConfigStore::getBool(const char* key,bool& out) const noexcept { try { std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return config::ConfigStatus::NOT_LOADED; const auto v=findPath(*impl_->snapshot,key?key:""); if(!v) return config::ConfigStatus::MISSING; if(v->kind!=JsonValue::Kind::Boolean) return config::ConfigStatus::INVALID_TYPE; out=v->boolean_value; return config::ConfigStatus::OK; } catch(...) { return config::ConfigStatus::INVALID_TYPE; } }
config::ConfigStatus ConfigStore::getString(const char* key,const char*& data,std::uint64_t& size) const noexcept { try { std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return config::ConfigStatus::NOT_LOADED; const auto v=findPath(*impl_->snapshot,key?key:""); if(!v) return config::ConfigStatus::MISSING; if(v->kind!=JsonValue::Kind::String) return config::ConfigStatus::INVALID_TYPE; data=v->string_value.data(); size=v->string_value.size(); return config::ConfigStatus::OK; } catch(...) { return config::ConfigStatus::INVALID_TYPE; } }
bool ConfigStore::has(const char* key) const noexcept { try { std::shared_lock lock(impl_->mutex); return impl_->snapshot&&findPath(*impl_->snapshot,key?key:""); } catch(...) { return false; } }
config::ConfigStatus ConfigStore::getArraySize(const char* key,std::uint64_t& size) const noexcept { try { std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return config::ConfigStatus::NOT_LOADED; const auto v=findPath(*impl_->snapshot,key?key:""); if(!v) return config::ConfigStatus::MISSING; if(v->kind!=JsonValue::Kind::Array) return config::ConfigStatus::INVALID_TYPE; size=v->array_value.size(); return config::ConfigStatus::OK; } catch(...) { return config::ConfigStatus::INVALID_TYPE; } }

template<class T> config::ConfigStatus arrayField(const JsonValue& root,const char* key,std::uint64_t index,const char* field,T& out) noexcept { const auto a=findPath(root,key?key:""); if(!a) return config::ConfigStatus::MISSING; if(a->kind!=JsonValue::Kind::Array||index>=a->array_value.size()) return config::ConfigStatus::OUT_OF_RANGE; const auto v=member(&a->array_value[static_cast<std::size_t>(index)],field?field:""); if(!v) return config::ConfigStatus::MISSING; if constexpr(std::is_same_v<T,std::int64_t>) { if(!integer(*v)) return config::ConfigStatus::INVALID_TYPE; out=v->integer_value; } else if constexpr(std::is_same_v<T,double>) { if(!number(*v)) return config::ConfigStatus::INVALID_TYPE; out=v->kind==JsonValue::Kind::Integer?static_cast<double>(v->integer_value):v->number_value; } else { if(v->kind!=JsonValue::Kind::Boolean) return config::ConfigStatus::INVALID_TYPE; out=v->boolean_value; } return config::ConfigStatus::OK; }
config::ConfigStatus ConfigStore::getIntAt(const char* key,std::uint64_t index,const char* field,std::int64_t& out) const noexcept { try { std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return config::ConfigStatus::NOT_LOADED; return arrayField(*impl_->snapshot,key,index,field,out); } catch(...) { return config::ConfigStatus::INVALID_TYPE; } }
config::ConfigStatus ConfigStore::getDoubleAt(const char* key,std::uint64_t index,const char* field,double& out) const noexcept { try { std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return config::ConfigStatus::NOT_LOADED; return arrayField(*impl_->snapshot,key,index,field,out); } catch(...) { return config::ConfigStatus::INVALID_TYPE; } }
config::ConfigStatus ConfigStore::getBoolAt(const char* key,std::uint64_t index,const char* field,bool& out) const noexcept { try { std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return config::ConfigStatus::NOT_LOADED; return arrayField(*impl_->snapshot,key,index,field,out); } catch(...) { return config::ConfigStatus::INVALID_TYPE; } }
config::ConfigStatus ConfigStore::getStringAt(const char* key,std::uint64_t index,const char* field,const char*& data,std::uint64_t& size) const noexcept { try { std::shared_lock lock(impl_->mutex); if(!impl_->snapshot) return config::ConfigStatus::NOT_LOADED; const auto a=findPath(*impl_->snapshot,key?key:""); if(!a) return config::ConfigStatus::MISSING; if(a->kind!=JsonValue::Kind::Array||index>=a->array_value.size()) return config::ConfigStatus::OUT_OF_RANGE; const auto v=member(&a->array_value[static_cast<std::size_t>(index)],field?field:""); if(!v) return config::ConfigStatus::MISSING; if(v->kind!=JsonValue::Kind::String) return config::ConfigStatus::INVALID_TYPE; data=v->string_value.data(); size=v->string_value.size(); return config::ConfigStatus::OK; } catch(...) { return config::ConfigStatus::INVALID_TYPE; } }
std::uint64_t ConfigStore::configVersion() const noexcept { try { std::shared_lock lock(impl_->mutex); return impl_->version; } catch(...) { return 0; } }
bool ConfigStore::isLoaded() const noexcept { try { std::shared_lock lock(impl_->mutex); return static_cast<bool>(impl_->snapshot); } catch(...) { return false; } }
runtime::ModuleInitResult LifecycleAdapter::initialize(const runtime::RuntimeContext& context) { const auto result=store_.load(context.configuration_path); runtime::ModuleInitResult output; output.module_name=name(); output.success=result.success; if(!result.success) output.error_message=result.diagnostics.empty()?errorName(result.error):result.diagnostics.front().key+": "+result.diagnostics.front().message; return output; }
void LifecycleAdapter::shutdown() noexcept {}
}




