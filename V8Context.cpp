#include "V8Context.h"

#include <pthread.h>
#include <time.h>

#include <sstream>

#ifndef INT32_MAX
#define INT32_MAX 0x7fffffff
#define INT32_MIN (-0x7fffffff-1)
#endif

#define L(...) fprintf(stderr, ##__VA_ARGS__)

using namespace v8;
using namespace std;

int V8Context::number = 0;
Platform* V8Context::platform = NULL;

#define V8_STRING(c_str) String::NewFromUtf8(isolate, c_str, NewStringType::kNormal).ToLocalChecked()

template <class TypeName>
inline Local<TypeName> StrongPersistentToLocal(
    const Persistent<TypeName>& persistent) {
  return *reinterpret_cast<Local<TypeName>*>(
      const_cast<Persistent<TypeName>*>(&persistent));
}

template <class TypeName>
inline Local<TypeName> WeakPersistentToLocal(
    Isolate* isolate,
    const Persistent<TypeName>& persistent) {
  return Local<TypeName>::New(isolate, persistent);
}

template <class TypeName>
inline Local<TypeName> PersistentToLocal(
    Isolate* isolate,
    const Persistent<TypeName>& persistent) {
  if (persistent.IsWeak()) {
    return WeakPersistentToLocal(isolate, persistent);
  } else {
    return StrongPersistentToLocal(persistent);
  }
}

void V8Context::set_perl_error(const TryCatch& try_catch) {
    Local<Message> msg = try_catch.Message();

    char message[1024];
    snprintf(
        message,
        1024,
        "%s at %s:%d:%d\n",
        (try_catch.HasTerminated() ? "timeout" : *(String::Utf8Value(try_catch.Exception()))),
        !msg.IsEmpty() ? *(String::Utf8Value(msg->GetScriptResourceName())) : "eval",
        !msg.IsEmpty() ? msg->GetLineNumber() : 0,
        !msg.IsEmpty() ? msg->GetStartColumn(): 0
    );

    sv_setpv(ERRSV, message);
    sv_utf8_upgrade(ERRSV);
}

Local<Value>
V8Context::check_perl_error() {
    if (!SvOK(ERRSV))
        return Local<Value>();

    const char *err = SvPV_nolen(ERRSV);

    if (err && strlen(err) > 0) {
        Local<String> error = V8_STRING(err);
        sv_setsv(ERRSV, &PL_sv_no);
        Local<Value> v = isolate->ThrowException(Exception::Error(error));
        return v;
    }

    return Local<Value>();
}

// Internally-used wrapper around coderefs
static IV
calculate_size(SV *sv) {
    return 1000;
    /*
     * There are horrible bugs in the current Devel::Size, so we can't do this
     * accurately. But if there weren't, this is how we'd do it!
    dSP;
    ENTER;
    SAVETMPS;

    PUSHMARK(SP);
    XPUSHs(sv);
    PUTBACK;
    int returned = call_pv("Devel::Size::total_size", G_SCALAR);
    if (returned != 1) {
        warn("Error calculating sv size");
        return 0;
    }

    SPAGAIN;
    IV size    = SvIV(POPs);
    PUTBACK;
    FREETMPS;
    LEAVE;

    return size;
    */
}

#define SETUP_PERL_CALL(PUSHSELF) \
    int len = args.Length(); \
\
    dSP; \
    ENTER; \
    SAVETMPS; \
\
    PUSHMARK(SP); \
\
    PUSHSELF; \
\
    for (int i = 0; i < len; i++) { \
        SV *arg = context->v82sv(args[i]); \
        mXPUSHs(arg); \
    } \
    PUTBACK;

#define CONVERT_PERL_RESULT() \
    Local<Value> error = context->check_perl_error(); \
\
    if (!error.IsEmpty()) { \
        FREETMPS; \
        LEAVE; \
        return error; \
    } \
    SPAGAIN; \
\
    Local<Value> v = context->sv2v8(POPs); \
\
    PUTBACK; \
    FREETMPS; \
    LEAVE; \
\
    return v;

void SvMap::add(Local<Object> object, long ptr) {
    objects.insert(
        pair<int, SimpleObjectData*>(
            object->GetIdentityHash(),
            new SimpleObjectData(object, ptr)
        )
    );
}

SV* SvMap::find(Local<Object> object) {
    int hash = object->GetIdentityHash();

    for (sv_map::const_iterator it = objects.find(hash); it != objects.end(), it->first == hash; it++)
        if (it->second->object->Equals(object))
            return newRV_inc(INT2PTR(SV*, it->second->ptr));

    return NULL;
}

ObjectData::ObjectData(V8Context* context_, Local<Object> object_, SV* sv_)
    : context(context_)
    , sv(sv_)
{
    object.Reset(context->get_isolate(), object_);

    if (!sv)
        return;

    ptr = PTR2IV(sv);

    context->register_object(this);
}

ObjectData::~ObjectData() {
    if (context) {
        context->remove_object(this);
        object.Reset();
    }
}

PerlObjectData::PerlObjectData(V8Context* context_, Local<Object> object_, SV* sv_)
    : ObjectData(context_, object_, sv_)
    , bytes(size())
{
    object.SetWeak(this, weak_callback, WeakCallbackType::kParameter);

    // PerlMethodData
    if (!sv)
        return;

    SvREFCNT_inc(sv);
    add_size(calculate_size(sv));
}

size_t PerlObjectData::size() {
    return sizeof(PerlObjectData);
}

void PerlObjectData::weak_callback(const WeakCallbackInfo<PerlObjectData> &data) {
    PerlObjectData *parameter = data.GetParameter();
    delete parameter;
}

PerlObjectData::~PerlObjectData() {
    add_size(-bytes);
    SvREFCNT_dec((SV*)sv);
}

V8ObjectData::V8ObjectData(V8Context* context_, Local<Object> object_, SV* sv_)
    : ObjectData(context_, object_, sv_)
{
    SV *iv = newSViv((IV) this);
    sv_magicext(sv, iv, PERL_MAGIC_ext, &vtable, "v8v8", 0);
    SvREFCNT_dec(iv); // refcnt is incremented by sv_magicext
}

MGVTBL V8ObjectData::vtable = {
    0,
    0,
    0,
    0,
    V8ObjectData::svt_free
};

int V8ObjectData::svt_free(pTHX_ SV* sv, MAGIC* mg) {
    delete (V8ObjectData*)SvIV(mg->mg_obj);
    return 0;
};

ObjectData* sv_object_data(SV* sv) {
    if (MAGIC *mg = mg_find(sv, PERL_MAGIC_ext)) {
        if (mg->mg_virtual == &V8ObjectData::vtable) {
            return (ObjectData*)SvIV(mg->mg_obj);
        }
    }
    return NULL;
}

class V8FunctionData : public V8ObjectData {
public:
    V8FunctionData(V8Context* context_, Local<Object> object_, SV* sv_)
        : V8ObjectData(context_, object_, sv_)
        , returns_list(object_->Has(StrongPersistentToLocal(context->perl_returns_list)))
    { }

    bool returns_list;
};

void PerlObjectData::add_size(size_t bytes_) {
    bytes += bytes_;
    context->get_isolate()->AdjustAmountOfExternalAllocatedMemory(bytes_);
}

Local<Object> get_function_wrapper(Isolate* isolate, PerlFunctionData* data) {
    return Local<Object>::Cast(FunctionTemplate::New(isolate, [] (const FunctionCallbackInfo<Value>& args) {
        PerlFunctionData *data = static_cast<PerlFunctionData*>(Local<External>::Cast(args.Data())->Value());

        args.GetReturnValue().Set(data->invoke(args));
    }, External::New(isolate, data))->GetFunction());
}


PerlFunctionData::PerlFunctionData(V8Context* context_, SV *cv)
    : PerlObjectData(
          context_,
          get_function_wrapper(context_->get_isolate(), this),
          cv
      )
   , rv(cv ? newRV_noinc(cv) : NULL)
{
}

size_t PerlFunctionData::size() {
    return sizeof(PerlFunctionData);
}

Local<Value> PerlFunctionData::invoke(const FunctionCallbackInfo<Value>& args) {
    SETUP_PERL_CALL();
    int count = call_sv(rv, G_SCALAR | G_EVAL);
    CONVERT_PERL_RESULT();
}

class PerlMethodData : public PerlFunctionData {
private:
    string name;
    virtual Local<Value> invoke(const FunctionCallbackInfo<Value> &args);
    virtual size_t size();

public:
    PerlMethodData(V8Context* context_, char* name_)
        : PerlFunctionData(context_, NULL)
        , name(name_)
    { }
};

Local<Value> PerlMethodData::invoke(const FunctionCallbackInfo<Value>& args) {
    SETUP_PERL_CALL(mXPUSHs(context->v82sv(args.This())))
    int count = call_method(name.c_str(), G_SCALAR | G_EVAL);
    CONVERT_PERL_RESULT()
}

size_t PerlMethodData::size() {
    return sizeof(PerlMethodData);
}

// V8Context class starts here

V8Context::V8Context(
    int time_limit,
    const char* flags,
    bool enable_blessing_,
    const char* bless_prefix_
)
    : time_limit_(time_limit),
      bless_prefix(bless_prefix_),
      enable_blessing(enable_blessing_)
{

    set_flags_from_string(flags);
    if (!platform) {
        V8::InitializeICU();
        platform = platform::CreateDefaultPlatform();
        V8::InitializePlatform(platform);
        V8::Initialize();
    }

    // Create a new Isolate and make it the current one.
    allocator = new ArrayBufferAllocator();
    Isolate::CreateParams create_params;
    create_params.array_buffer_allocator = allocator;
    isolate = Isolate::New(create_params);
    isolate->Enter();
    HandleScope handle_scope(isolate);
    Local<Context> localContext = Context::New(isolate);
    context.Reset(isolate, localContext);

    Context::Scope context_scope(localContext);

    perl_returns_list.Reset(isolate, V8_STRING("__perlReturnsList"));
    string_wrap.Reset(isolate, V8_STRING("wrap"));

    number++;
}

void V8Context::register_object(ObjectData* data) {
    seen_perl[data->ptr] = data;
    PersistentToLocal(isolate, data->object)->SetHiddenValue(StrongPersistentToLocal(string_wrap), External::New(isolate, data));
}

void V8Context::remove_object(ObjectData* data) {
    ObjectDataMap::iterator it = seen_perl.find(data->ptr);
    if (it != seen_perl.end())
        seen_perl.erase(it);
    // is it ok?
    //    PersistentToLocal(isolate, data->object)->DeleteHiddenValue(StrongPersistentToLocal(string_wrap));
}

Isolate *V8Context::get_isolate() const {
    return isolate;
}

V8Context::~V8Context() {
    for (ObjectDataMap::iterator it = seen_perl.begin(); it != seen_perl.end(); it++) {
        it->second->context = NULL;
    }
    seen_perl.clear();

    for (ObjectMap::iterator it = prototypes.begin(); it != prototypes.end(); it++) {
      it->second->Reset();
      delete it->second;
    }
    context.Reset();
    isolate->Exit();
    isolate->Dispose();
// fix it?
//    V8::Dispose();
//    V8::ShutdownPlatform();
//    delete platform;
    delete allocator;
//    while(!IdleNotification()); // force garbage collection
}

Local<Context> V8Context::GlobalContext() {
    return StrongPersistentToLocal(context);
}

void
V8Context::bind(const char *name, SV *thing) {
    HandleScope scope(isolate);
    Local<Context> local_context(GlobalContext());
    Context::Scope context_scope(local_context);

    local_context->Global()->Set(String::NewFromUtf8(isolate, name), sv2v8(thing));
}

void
V8Context::bind_ro(const char *name, SV *thing) {
    HandleScope scope(isolate);
    Local<Context> local_context(GlobalContext());
    Context::Scope context_scope(local_context);

    local_context->Global()->ForceSet(String::NewFromUtf8(isolate, name), sv2v8(thing),
        PropertyAttribute(ReadOnly | DontDelete));
}

void V8Context::name_global(const char *name) {
    HandleScope scope(isolate);
    Local<Context> local_context(GlobalContext());
    Context::Scope context_scope(local_context);

    local_context->Global()->ForceSet(String::NewFromUtf8(isolate, name), local_context->Global(),
        PropertyAttribute(ReadOnly | DontDelete));
}

// I fucking hate pthreads, this lacks error handling, but hopefully works.
class thread_canceller {
public:
    thread_canceller(Isolate* isolate, int sec)
        : sec_(sec)
        , isolate_(isolate)
    {
        if (sec_) {
            pthread_cond_init(&cond_, NULL);
            pthread_mutex_init(&mutex_, NULL);
            pthread_mutex_lock(&mutex_); // passed locked to canceller
            pthread_create(&id_, NULL, canceller, this);
        }
    }

    ~thread_canceller() {
        if (sec_) {
            pthread_mutex_lock(&mutex_);
            pthread_cond_signal(&cond_);
            pthread_mutex_unlock(&mutex_);
            void *ret;
            pthread_join(id_, &ret);
            pthread_mutex_destroy(&mutex_);
            pthread_cond_destroy(&cond_);
        }
    }

private:

    static void* canceller(void* this_) {
        thread_canceller* me = static_cast<thread_canceller*>(this_);
        struct timeval tv;
        struct timespec ts;
        gettimeofday(&tv, NULL);
        ts.tv_sec = tv.tv_sec + me->sec_;
        ts.tv_nsec = tv.tv_usec * 1000;

        if (pthread_cond_timedwait(&me->cond_, &me->mutex_, &ts) == ETIMEDOUT) {
            me->isolate_->TerminateExecution();
        }
        pthread_mutex_unlock(&me->mutex_);
        return NULL;
    }

    pthread_t id_;
    pthread_cond_t cond_;
    pthread_mutex_t mutex_;
    int sec_;
    Isolate* isolate_;
};

SV*
V8Context::eval(SV* source, SV* origin) {
    HandleScope scope(isolate);
    Local<Context> local_context(GlobalContext());
    Context::Scope context_scope(local_context);

    TryCatch try_catch(isolate);

    // V8 expects everything in UTF-8, ensure SVs are upgraded.
    sv_utf8_upgrade(source);
    Local<Script> script = Script::Compile(
        sv2v8str(source),
        origin ? sv2v8str(origin) : String::NewFromUtf8(isolate, "eval")
    );

    if (try_catch.HasCaught()) {
        set_perl_error(try_catch);
        return &PL_sv_undef;
    } else {
        thread_canceller canceller(isolate, time_limit_);
        Local<Value> val = script->Run();

        if (val.IsEmpty()) {
            set_perl_error(try_catch);
            return &PL_sv_undef;
        } else {
            sv_setsv(ERRSV,&PL_sv_undef);
            if (GIMME_V == G_VOID) {
                return &PL_sv_undef;
            }
            return v82sv(val);
        }
    }
}

Local<Value>
V8Context::sv2v8(SV *sv, HandleMap& seen) {
    if (SvROK(sv))
        return rv2v8(sv, seen);
    if (SvPOK(sv)) {
        // Upgrade string to UTF-8 if needed
        char *utf8 = SvPVutf8_nolen(sv);
        return String::NewFromUtf8(isolate, utf8, NewStringType::kNormal, SvCUR(sv)).ToLocalChecked();
    }
    if (SvIOK(sv)) {
        IV v = SvIV(sv);
        return (v <= INT32_MAX && v >= INT32_MIN) ? (Local<Number>)Integer::New(isolate, v) : Number::New(isolate, SvNV(sv));
    }
    if (SvNOK(sv))
        return Number::New(isolate, SvNV(sv));
    if (!SvOK(sv))
        return Undefined(isolate);

    warn("Unknown sv type in sv2v8");
    return Undefined(isolate);
}

Local<Value>
V8Context::sv2v8(SV *sv) {
    HandleMap seen;
    return sv2v8(sv, seen);
}

Local<String> V8Context::sv2v8str(SV* sv)
{
    // Upgrade string to UTF-8 if needed
    char *utf8 = SvPVutf8_nolen(sv);
    return String::NewFromUtf8(isolate, utf8, NewStringType::kNormal, SvCUR(sv)).ToLocalChecked();
}

SV* V8Context::seen_v8(Local<Object> object) {
    Local<Value> wrap = object->GetHiddenValue(StrongPersistentToLocal(string_wrap));
    if (wrap.IsEmpty())
        return NULL;

    ObjectData* data = (ObjectData*)(Local<External>::Cast(wrap)->Value());
    return newRV(data->sv);
}

SV *
V8Context::v82sv(Local<Value> value, SvMap& seen) {
    if (value->IsUndefined())
        return &PL_sv_undef;

    if (value->IsNull())
        return &PL_sv_undef;

    if (value->IsInt32())
        return newSViv(value->Int32Value());

    if (value->IsBoolean())
        return newSVuv(value->Uint32Value());

    if (value->IsNumber())
        return newSVnv(value->NumberValue());

    if (value->IsString()) {
        String::Utf8Value str(value);
        SV *sv = newSVpvn(*str, str.length());
        sv_utf8_decode(sv);
        return sv;
    }

    if (value->IsArray() || value->IsObject() || value->IsFunction()) {
        Local<Object> object = value->ToObject();

        if (SV *cached = seen_v8(object))
            return cached;

        if (value->IsFunction()) {
            Local<Function> fn = Local<Function>::Cast(value);
            return function2sv(fn);
        }

        if (SV* cached = seen.find(object))
            return cached;

        if (value->IsArray()) {
            Local<Array> array = Local<Array>::Cast(value);
            return array2sv(array, seen);
        }

        if (value->IsObject()) {
            Local<Object> object = Local<Object>::Cast(value);
            return object2sv(object, seen);
        }
    }

    warn("Unknown v8 value in v82sv");
    return &PL_sv_undef;
}

SV *
V8Context::v82sv(Local<Value> value) {
    SvMap seen;
    return v82sv(value, seen);
}

void
V8Context::fill_prototype(Local<Object> prototype, HV* stash) {
    HE *he;
    while (he = hv_iternext(stash)) {
        SV *key = HeSVKEY_force(he);
        Local<String> name = String::NewFromUtf8(isolate, SvPV_nolen(key));

        if (prototype->Has(name))
            continue;

        prototype->Set(name, PersistentToLocal(isolate, (new PerlMethodData(this, SvPV_nolen(key)))->object));
    }
}

#if PERL_VERSION > 8
Local<Object>
V8Context::get_prototype(SV *sv) {
    HV *stash = SvSTASH(sv);
    char *package = HvNAME(stash);

    std::string pkg(package);
    ObjectMap::iterator it;

    Local<Object> prototype = Object::New(isolate);

    it = prototypes.find(pkg);
    if (it != prototypes.end()) {
        prototype = Local<Object>::New(isolate, *it->second);
    }
    else {
        prototype = Object::New(isolate);
        prototypes[pkg] = new Persistent<Object>(isolate, prototype);

        if (AV *isa = mro_get_linear_isa(stash)) {
            for (int i = 0; i <= av_len(isa); i++) {
                SV **sv = av_fetch(isa, i, 0);
                HV *stash = gv_stashsv(*sv, 0);
                fill_prototype(prototype, stash);
            }
        }
    }

    return prototype;
}
#endif

Local<Value>
V8Context::rv2v8(SV *rv, HandleMap& seen) {
    SV* sv = SvRV(rv);
    long ptr = PTR2IV(sv);

    {
        ObjectDataMap::iterator it = seen_perl.find(ptr);
        if (it != seen_perl.end())
            return PersistentToLocal(isolate, it->second->object);
    }

    {
        HandleMap::const_iterator it = seen.find(ptr);
        if (it != seen.end())
            return it->second;
    }

#if PERL_VERSION > 8
    if (SvOBJECT(sv))
        return blessed2object(sv);
#endif

    unsigned t = SvTYPE(sv);

    if (t == SVt_PVAV)
        return av2array((AV*)sv, seen, ptr);

    if (t == SVt_PVHV)
        return hv2object((HV*)sv, seen, ptr);

    if (t == SVt_PVCV)
        return cv2function((CV*)sv);

    warn("Unknown reference type in sv2v8()");
    return Undefined(isolate);
}

#if PERL_VERSION > 8
Local<Object>
V8Context::blessed2object(SV *sv) {
    Local<Object> object = Object::New(isolate);
    object->SetPrototype(get_prototype(sv));

    return PersistentToLocal(isolate, (new PerlObjectData(this, object, sv))->object);
}
#endif

Local<Array>
V8Context::av2array(AV *av, HandleMap& seen, long ptr) {
    I32 i, len = av_len(av) + 1;
    Local<Array> array = Array::New(isolate, len);
    seen[ptr] = array;
    for (i = 0; i < len; i++) {
        if (SV** sv = av_fetch(av, i, 0)) {
            array->Set(Integer::New(isolate, i), sv2v8(*sv, seen));
        }
    }
    return array;
}

Local<Object>
V8Context::hv2object(HV *hv, HandleMap& seen, long ptr) {
    I32 len;
    char *key;
    SV *val;

    hv_iterinit(hv);
    Local<Object> object = Object::New(isolate);
    seen[ptr] = object;
    while (val = hv_iternextsv(hv, &key, &len)) {
        object->Set(String::NewFromUtf8(isolate, key, NewStringType::kNormal, len).ToLocalChecked(), sv2v8(val, seen));
    }
    return object;
}

Local<Object>
V8Context::cv2function(CV *cv) {
    return PersistentToLocal(isolate, (new PerlFunctionData(this, (SV*)cv))->object);
}

SV*
V8Context::array2sv(Local<Array> array, SvMap& seen) {
    AV *av = newAV();
    SV *rv = newRV_noinc((SV*)av);
    SvREFCNT_inc(rv);

    seen.add(array, PTR2IV(av));

    for (int i = 0; i < array->Length(); i++) {
        Local<Value> elementVal = array->Get(Integer::New(isolate, i));
        av_push(av, v82sv(elementVal, seen));
    }
    return rv;
}

SV *
V8Context::object2sv(Local<Object> obj, SvMap& seen) {
    if (enable_blessing && obj->Has(String::NewFromUtf8(isolate, "__perlPackage"))) {
        return object2blessed(obj);
    }

    HV *hv = newHV();
    SV *rv = newRV_noinc((SV*)hv);
    SvREFCNT_inc(rv);

    seen.add(obj, PTR2IV(hv));

    Local<Array> properties = obj->GetPropertyNames();
    for (int i = 0; i < properties->Length(); i++) {
        Local<Integer> propertyIndex = Integer::New(isolate, i);
        Local<String> propertyName = Local<String>::Cast( properties->Get( propertyIndex ) );
        String::Utf8Value propertyNameUTF8( propertyName );

        Local<Value> propertyValue = obj->Get( propertyName );
        if (*propertyValue)
            hv_store(hv, *propertyNameUTF8, 0 - propertyNameUTF8.length(), v82sv(propertyValue, seen), 0);
    }
    return rv;
}

static void
my_gv_setsv(pTHX_ GV* const gv, SV* const sv){
    ENTER;
    SAVETMPS;

    sv_setsv_mg((SV*)gv, sv_2mortal(newRV_inc((sv))));

    FREETMPS;
    LEAVE;
}

#ifdef dVAR
    #define DVAR dVAR;
#endif

#define SETUP_V8_CALL(ARGS_OFFSET) \
    DVAR \
    dXSARGS; \
\
    bool die = false; \
    int count = 1; \
\
    { \
        /* We have to do all this inside a block so that all the proper \
         * destuctors are called if we need to croak. If we just croak in the \
         * middle of the block, v8 will segfault at program exit. */ \
        V8FunctionData* data = (V8FunctionData*)sv_object_data((SV*)cv); \
        if (data->context) { \
\
            V8Context      *self = data->context; \
            Isolate        *isolate = self->get_isolate(); \
\
            HandleScope scope(isolate); \
            Local<Context> ctx(self->GlobalContext()); \
            Context::Scope context_scope(ctx); \
\
            TryCatch        try_catch(isolate); \
            Local<Value>   argv[items - ARGS_OFFSET]; \
\
            for (I32 i = ARGS_OFFSET; i < items; i++) { \
                argv[i - ARGS_OFFSET] = self->sv2v8(ST(i)); \
            }

#define CONVERT_V8_RESULT(POP) \
            if (try_catch.HasCaught()) { \
                data->context->set_perl_error(try_catch); \
                die = true; \
            } \
            else { \
                if (data->returns_list && GIMME_V == G_ARRAY && result->IsArray()) { \
                    Local<Array> array = Local<Array>::Cast(result); \
                    if (GIMME_V == G_ARRAY) { \
                        count = array->Length(); \
                        EXTEND(SP, count - items); \
                        for (int i = 0; i < count; i++) { \
                            ST(i) = sv_2mortal(self->v82sv(array->Get(Integer::New(isolate, i)))); \
                        } \
                    } \
                    else { \
                        ST(0) = sv_2mortal(newSViv(array->Length())); \
                    } \
                } \
                else { \
                    ST(0) = sv_2mortal(self->v82sv(result)); \
                } \
            } \
        } \
        else {\
            die = true; \
            sv_setpv(ERRSV, "Fatal error: V8 context is no more"); \
            sv_utf8_upgrade(ERRSV); \
        } \
    } \
\
    if (die) \
        croak(NULL); \
\
XSRETURN(count);

XS(v8closure) {
    SETUP_V8_CALL(0)
    Local<Value> result = Local<Function>::Cast(PersistentToLocal(isolate, data->object))->Call(ctx->Global(), items, argv);
    CONVERT_V8_RESULT()
}

XS(v8method) {
    SETUP_V8_CALL(1)
    V8ObjectData* This = (V8ObjectData*)SvIV((SV*)SvRV(ST(0)));
    Local<Value> result = Local<Function>::Cast(PersistentToLocal(isolate, data->object))->Call(PersistentToLocal(isolate, This->object), items - 1, argv);
    CONVERT_V8_RESULT(POPs)
}

SV*
V8Context::function2sv(Local<Function> fn) {
    CV          *code = newXS(NULL, v8closure, __FILE__);
    V8FunctionData *data = new V8FunctionData(this, fn->ToObject(), (SV*)code);
    return newRV_noinc((SV*)code);
}

SV*
V8Context::object2blessed(Local<Object> obj) {
    char package[128];

    snprintf(
        package,
        128,
        "%s%s::N%d",
        bless_prefix.c_str(),
        *String::Utf8Value(obj->Get(String::NewFromUtf8(isolate, "__perlPackage"))->ToString()),
        number
    );

    HV *stash = gv_stashpv(package, 0);

    if (!stash) {
        Local<Object> prototype = obj->GetPrototype()->ToObject();

        stash = gv_stashpv(package, GV_ADD);

        Local<Array> properties = prototype->GetPropertyNames();
        for (int i = 0; i < properties->Length(); i++) {
            Local<String> name = properties->Get(i)->ToString();
            Local<Value> property = prototype->Get(name);

            if (!property->IsFunction())
                continue;

            Local<Function> fn = Local<Function>::Cast(property);

            CV *code = newXS(NULL, v8method, __FILE__);
            V8ObjectData *data = new V8FunctionData(this, fn, (SV*)code);

            GV* gv = (GV*)*hv_fetch(stash, *String::Utf8Value(name), name->Length(), TRUE);
            gv_init(gv, stash, *String::Utf8Value(name), name->Length(), GV_ADDMULTI); /* vivify */
            my_gv_setsv(aTHX_ gv, (SV*)code);
        }
    }

    SV* rv = newSV(0);
    SV* sv = newSVrv(rv, package);
    V8ObjectData *data = new V8ObjectData(this, obj, sv);
    sv_setiv(sv, PTR2IV(data));

    return rv;
}

bool
V8Context::idle_notification() {
    /*
    HeapStatistics hs;
    GetHeapStatistics(&hs);
    L(
        "%d %d %d\n",
        hs.total_heap_size(),
        hs.total_heap_size_executable(),
        hs.used_heap_size()
    );
    */
    return isolate->IdleNotification(100);
}

int
V8Context::adjust_amount_of_external_allocated_memory(int change_in_bytes) {
    return isolate->AdjustAmountOfExternalAllocatedMemory(change_in_bytes);
}

void
V8Context::set_flags_from_string(const char *str) {
    V8::SetFlagsFromString(str, strlen(str));
    // for testing only
    if (strstr(str, "--expose-gc")) {
        HandleScope scope(isolate);
        Local<Context> local_context(GlobalContext());
        Context::Scope context_scope(local_context);

        Local<Function> gc = FunctionTemplate::New(isolate, [] (const FunctionCallbackInfo<Value>& args) {
            args.GetIsolate()->RequestGarbageCollectionForTesting(Isolate::kFullGarbageCollection);
        })->GetFunction();
        local_context->Global()->Set(String::NewFromUtf8(isolate, "gc"), gc);
    }
}
