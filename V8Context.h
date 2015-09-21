#ifndef _V8Context_h_
#define _V8Context_h_

#include <libplatform/libplatform.h>
#include <v8.h>

#include <vector>
#include <map>
#include <string>

#ifdef __cplusplus
extern "C" {
#include <EXTERN.h>
#include <perl.h>
#include <XSUB.h>
#include "ppport.h"
}
#endif
#undef New
#undef Null
#undef do_open
#undef do_close

using namespace v8;
using namespace std;

typedef map<string, Persistent<Object> *> ObjectMap;

class SimpleObjectData {
public:
    Local<Object> object;
    long ptr;

    SimpleObjectData(Local<Object> object_, long ptr_)
        : object(object_)
        , ptr(ptr_)
    { }
};

class SvMap {
    typedef multimap<int, SimpleObjectData*> sv_map;
    sv_map objects;

public:
    SvMap () { }

    ~SvMap() {
        for (sv_map::iterator it = objects.begin(); it != objects.end(); it++)
            delete it->second;
    }

    void add(Local<Object> object, long ptr);
    SV* find(Local<Object> object);
};

typedef map<int, Local<Value> > HandleMap;

class V8Context;

class ObjectData {
public:
    V8Context* context;
    Persistent<Object> object;
    SV* sv;
    long ptr;

    ObjectData(V8Context* context_, Local<Object> object_, SV* sv);
    virtual ~ObjectData();
};

class V8ObjectData : public ObjectData {
public:
    V8ObjectData(V8Context* context_, Local<Object> object_, SV* sv_);

    static MGVTBL vtable;
    static int svt_free(pTHX_ SV*, MAGIC*);
};

class PerlObjectData : public ObjectData {
    size_t bytes;

public:
    PerlObjectData(V8Context* context_, Local<Object> object_, SV* sv_);
    virtual ~PerlObjectData();

    virtual size_t size();
    void add_size(size_t bytes_);

    static void weak_callback(const WeakCallbackInfo<PerlObjectData> &data);
};

class PerlFunctionData : public PerlObjectData {
private:
    SV *rv;

protected:
    virtual size_t size();

public:
    PerlFunctionData(V8Context* context_, SV *cv);

    virtual Local<Value> invoke(const FunctionCallbackInfo<Value> &args);
};

typedef map<int, ObjectData*> ObjectDataMap;

class ArrayBufferAllocator : public ArrayBuffer::Allocator {
 public:
  virtual void* Allocate(size_t length) {
    void* data = AllocateUninitialized(length);
    return data == NULL ? data : memset(data, 0, length);
  }
  virtual void* AllocateUninitialized(size_t length) { return malloc(length); }
  virtual void Free(void* data, size_t) { free(data); }
};

class V8Context {
    public:
        V8Context(
            int time_limit = 0,
            const char* flags = NULL,
            bool enable_blessing = false,
            const char* bless_prefix = NULL
        );
        ~V8Context();

        void bind(const char*, SV*);
        void bind_ro(const char*, SV*);
        SV* eval(SV* source, SV* origin = NULL);
        bool idle_notification();
        int adjust_amount_of_external_allocated_memory(int bytes);
        void set_flags_from_string(const char *str);
        void name_global(const char *str);

        Local<Value> sv2v8(SV*);
        SV*          v82sv(Local<Value>);
        Local<Value> check_perl_error();
        void         set_perl_error(const TryCatch& try_catch);

        void register_object(ObjectData* data);
        void remove_object(ObjectData* data);
        Isolate* get_isolate() const;

        Persistent<String> perl_returns_list;

        Local<Context> GlobalContext();

    private:
        Isolate* isolate;
        static Platform* platform;
        ArrayBufferAllocator *allocator;
        Persistent<Context> context;

        Local<Value> sv2v8(SV*, HandleMap& seen);
        SV*          v82sv(Local<Value>, SvMap& seen);

        Local<Value> rv2v8(SV*, HandleMap& seen);
        Local<Array> av2array(AV*, HandleMap& seen, long ptr);
        Local<Object>   hv2object(HV*, HandleMap& seen, long ptr);
        Local<Object> cv2function(CV*);
        Local<String> sv2v8str(SV* sv);
        Local<Object>   blessed2object(SV *sv);

        SV* array2sv(Local<Array>, SvMap& seen);
        SV* object2sv(Local<Object>, SvMap& seen);
        SV* object2blessed(Local<Object>);
        SV* function2sv(Local<Function>);

        Persistent<String> string_wrap;

        void fill_prototype(Local<Object> prototype, HV* stash);
        Local<Object> get_prototype(SV* sv);

        ObjectMap prototypes;

        ObjectDataMap seen_perl;
        SV* seen_v8(Local<Object> object);

        int time_limit_;
        string bless_prefix;
        bool enable_blessing;
        static int number;

};

#endif
